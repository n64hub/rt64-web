//
// RT64
//

#include "rt64_render_target_manager.h"

#include <algorithm>

#include "xxHash/xxh3.h"

namespace RT64 {
    // RenderTargetKey

    RenderTargetKey::RenderTargetKey(uint32_t address, uint32_t width, uint32_t siz, Framebuffer::Type fbType) {
        this->address = address;
        this->width = width;
        this->siz = siz;
        this->fbType = fbType;
    }

    uint64_t RenderTargetKey::hash() const {
        return XXH3_64bits(this, sizeof(RenderTargetKey));
    }

    bool RenderTargetKey::isEmpty() const {
        return (width == 0) || (fbType == Framebuffer::Type::None);
    }
    
    // RenderTargetManager

    RenderTargetManager::RenderTargetManager() { }

    void RenderTargetManager::setMultisampling(const RenderMultisampling &multisampling) {
        this->multisampling = multisampling;
    }

    void RenderTargetManager::setUsesHDR(bool usesHDR) {
        this->usesHDR = usesHDR;
    }

    RenderTarget &RenderTargetManager::get(const RenderTargetKey &key, bool ignoreOverrides) {
        const uint64_t keyHash = key.hash();
        if (!ignoreOverrides) {
            auto overrideIt = overrideMap.find(keyHash);
            if (overrideIt != overrideMap.end()) {
                return *overrideIt->second;
            }
        }

        std::unique_ptr<RenderTarget> &target = targetMap[keyHash];
        if (target != nullptr) {
            return *target;
        }

        // A target discard() set aside and freeDiscarded() hasn't freed yet is the same target again.
        auto discardedIt = discardedMap.find(keyHash);
        if (discardedIt != discardedMap.end()) {
            target = std::move(discardedIt->second.target);
            discardedMap.erase(discardedIt);
            return *target;
        }

        target = std::make_unique<RenderTarget>(key.address, key.fbType, multisampling, usesHDR);
        return *target;
    }
    
    void RenderTargetManager::destroyAll() {
        targetMap.clear();
        discardedMap.clear();
    }

    void RenderTargetManager::setOverride(const RenderTargetKey &key, RenderTarget *target) {
        overrideMap[key.hash()] = target;
    }

    void RenderTargetManager::removeOverride(const RenderTargetKey &key) {
        overrideMap.erase(key.hash());
    }

    // RT64_FREE_DISCARDED_TARGETS: the targets of the framebuffers at the addresses the framebuffer manager discarded,
    // their RAM overwritten by the CPU, are set aside. One get() asks for again comes back as it was; the rest are
    // freed by freeDiscarded(), so a game that moves its framebuffers (Perfect Dark allocates them and its depth buffer
    // from each level's memory) doesn't keep every level's targets on the GPU.
    void RenderTargetManager::discard(const std::vector<uint32_t> &addresses, uint64_t frame) {
        if (addresses.empty()) {
            return;
        }

        auto it = targetMap.begin();
        while (it != targetMap.end()) {
            const uint32_t address = it->second->addressForName;
            if (std::find(addresses.begin(), addresses.end(), address) != addresses.end()) {
                Discarded &discarded = discardedMap[it->first];
                discarded.target = std::move(it->second);
                discarded.frame = frame;
                it = targetMap.erase(it);
            }
            else {
                it++;
            }
        }
    }

    void RenderTargetManager::freeDiscarded(uint64_t frame, uint64_t minimumAge, RenderFramebufferManager *framebufferManager) {
        auto it = discardedMap.begin();
        while (it != discardedMap.end()) {
            if ((frame - it->second.frame) >= minimumAge) {
                framebufferManager->destroyAllWithRenderTarget(it->second.target.get());
                it = discardedMap.erase(it);
            }
            else {
                it++;
            }
        }
    }

    // RenderFramebufferKey

    uint64_t RenderFramebufferKey::hash() const {
        return XXH3_64bits(this, sizeof(RenderFramebufferKey));
    }

    // RenderFramebufferStorage

    void RenderFramebufferStorage::setup(RenderDevice *device, const RenderFramebufferKey &framebufferKey, RenderTarget *colorTarget, RenderTarget *depthTarget) {
        if ((depthTarget != nullptr) && (colorTarget != nullptr)) {
            assert((depthTarget->width >= colorTarget->width) && (depthTarget->height >= colorTarget->height) && "Depth target must be equal or bigger to the color target.");
        }

        this->colorTarget = colorTarget;
        this->depthTarget = depthTarget;
        this->framebufferKey = framebufferKey;
        
        colorTargetRevision = (colorTarget != nullptr) ? colorTarget->textureRevision : 0;
        depthTargetRevision = (depthTarget != nullptr) ? depthTarget->textureRevision : 0;

        RenderFramebufferDesc framebufferDesc;
        const RenderTexture *colorTargetPtr = (colorTarget != nullptr) ? colorTarget->texture.get() : nullptr;
        framebufferDesc.colorAttachments = &colorTargetPtr;
        framebufferDesc.colorAttachmentsCount = (colorTarget != nullptr) ? 1 : 0;
        framebufferDesc.depthAttachment = (depthTarget != nullptr) ? depthTarget->texture.get() : nullptr;
        colorDepthWrite = device->createFramebuffer(framebufferDesc);

        if (framebufferDesc.depthAttachment != nullptr) {
            // Create the depth read-only version of the framebuffer.
            framebufferDesc.depthAttachmentReadOnly = true;
            colorWriteDepthRead = device->createFramebuffer(framebufferDesc);
        }
    }

    // RenderFramebufferManager

    RenderFramebufferManager::RenderFramebufferManager(RenderDevice *device) {
        assert(device != nullptr);

        this->device = device;
    }

    RenderFramebufferStorage &RenderFramebufferManager::get(const RenderFramebufferKey &framebufferKey, RenderTarget *colorTarget, RenderTarget *depthTarget) {
        assert(framebufferKey.colorTargetKey.isEmpty() || (colorTarget != nullptr));
        assert(framebufferKey.depthTargetKey.isEmpty() || (depthTarget != nullptr));

        const uint64_t keyHash = framebufferKey.hash();
        RenderFramebufferStorage &storage = storageMap[keyHash];
        if (storage.colorTarget != nullptr || storage.depthTarget != nullptr) {
            assert((storage.colorTarget == colorTarget) && "Storage does not match the requested color target.");
            assert((storage.depthTarget == depthTarget) && "Storage does not match the requested depth target.");
            assert((colorTarget == nullptr) || (storage.colorTargetRevision == colorTarget->textureRevision) && "Storage does not match the requested color texture revision.");
            assert((depthTarget == nullptr) || (storage.depthTargetRevision == depthTarget->textureRevision) && "Storage does not match the requested depth texture revision.");
            return storage;
        }

        storage.setup(device, framebufferKey, colorTarget, depthTarget);

        return storage;
    }

    void RenderFramebufferManager::destroyAll() {
        storageMap.clear();
    }

    void RenderFramebufferManager::destroyAllWithRenderTarget(RenderTarget *renderTarget) {
        auto it = storageMap.begin();
        while (it != storageMap.end()) {
            if ((it->second.colorTarget == renderTarget) || (it->second.depthTarget == renderTarget)) {
                it = storageMap.erase(it);
            }
            else {
                it++;
            }
        }
    }

#if RT64_WGSL
    // DummyDepthTargets

    RenderTarget *DummyDepthTargets::get(RenderWorker *worker, const RenderTarget *colorTarget, uint64_t frame) {
        assert(colorTarget != nullptr);

        for (Dummy &dummy : dummies) {
            if ((dummy.target->width == colorTarget->width) && (dummy.target->height == colorTarget->height)) {
                dummy.frame = frame;
                return dummy.target.get();
            }
        }

        Dummy dummy;
        dummy.target = std::make_unique<RenderTarget>(0, Framebuffer::Type::Depth, colorTarget->multisampling, colorTarget->usesHDR);
        dummy.target->setupDepth(worker, colorTarget->width, colorTarget->height);
        dummy.frame = frame;
        dummies.emplace_back(std::move(dummy));
        return dummies.back().target.get();
    }

    void DummyDepthTargets::freeUnused(uint64_t frame, RenderFramebufferManager *framebufferManager) {
        auto it = dummies.begin();
        while (it != dummies.end()) {
            if ((it->frame + UnusedFrames) < frame) {
                framebufferManager->destroyAllWithRenderTarget(it->target.get());
                it = dummies.erase(it);
            }
            else {
                it++;
            }
        }
    }
#endif
};