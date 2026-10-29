//
// RT64
//

#include "rt64_framebuffer_changes.h"

#include "render/rt64_render_target.h"

namespace RT64 {
    // FramebufferChange

    FramebufferChange::FramebufferChange() {
        id = 0;
        type = Type::Color;
        width = 0;
        height = 0;
        used = false;
    }

    FramebufferChange::~FramebufferChange() { }

    // FramebufferChangePool

    FramebufferChangePool::FramebufferChangePool() {
        newId = 1;
    }

    FramebufferChangePool::~FramebufferChangePool() { }

    void FramebufferChangePool::reset() {
        for (auto &changes : changesMap) {
            changes.second.used = false;
        }
    }

    FramebufferChange &FramebufferChangePool::use(RenderWorker *renderWorker, FramebufferChange::Type type, uint32_t width, uint32_t height, bool usesHDR) {
        // To increase the chances of reusing buffers, we extend the width and height to a multiple of 32.
        const uint32_t Alignment = 32;
        uint32_t alignedWidth = ((width / Alignment) + ((width % Alignment) ? 1 : 0)) * Alignment;
        uint32_t alignedHeight = ((height / Alignment) + ((height % Alignment) ? 1 : 0)) * Alignment;

        // Find a compatible changes buffer to use.
        for (auto &changes : changesMap) {
            if (changes.second.used) {
                continue;
            }

            if ((changes.second.type == type) &&
                (changes.second.width == alignedWidth) &&
                (changes.second.height == alignedHeight))
            {
                changes.second.used = true;
                return changes.second;
            }
        }

        uint64_t changesId = newId++;
        auto &changes = changesMap[changesId];
        changes.id = changesId;
        changes.type = type;
        changes.width = alignedWidth;
        changes.height = alignedHeight;
        changes.used = true;

        RenderFormat pixelFormat;
        switch (type) {
        case FramebufferChange::Type::Color:
            pixelFormat = RenderTarget::colorBufferFormat(usesHDR);
            break;
        case FramebufferChange::Type::Depth:
            pixelFormat = RenderFormat::R32_FLOAT;
            break;
        default:
            assert(false && "Invalid framebuffer change type.");
            break;
        }

        changes.pixelTexture = renderWorker->device->createTexture(RenderTextureDesc::Texture2D(alignedWidth, alignedHeight, 1, pixelFormat, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
#   if RT64_WGSL
        // WebGPU has no 8-bit storage textures (the shaders declare it r32ui there).
        const RenderFormat booleanFormat = RenderFormat::R32_UINT;
#   else
        const RenderFormat booleanFormat = RenderFormat::R8_UINT;
#   endif
        changes.booleanTexture = renderWorker->device->createTexture(RenderTextureDesc::Texture2D(alignedWidth, alignedHeight, 1, booleanFormat, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
        changes.drawDescSet = std::make_unique<FramebufferDrawChangesDescriptorSet>(renderWorker->device);
        changes.drawDescSet->setTexture(changes.drawDescSet->gColor, changes.pixelTexture.get(), RenderTextureLayout::SHADER_READ);
        changes.drawDescSet->setTexture(changes.drawDescSet->gDepth, changes.pixelTexture.get(), RenderTextureLayout::SHADER_READ);
        changes.drawDescSet->setTexture(changes.drawDescSet->gBoolean, changes.booleanTexture.get(), RenderTextureLayout::SHADER_READ);
        changes.readChangesSet = std::make_unique<FramebufferReadChangesDescriptorChangesSet>(renderWorker->device);
#   if RT64_WGSL
        // WebGPU checks a storage texture's format against the shader's for every binding a shader can reach, so the
        // output of the type this change isn't gets a texture of its own format, which the shader never writes.
        const RenderFormat otherTypeFormat = (type == FramebufferChange::Type::Color) ? RenderFormat::R32_FLOAT : RenderTarget::colorBufferFormat(usesHDR);
        changes.otherTypeTexture = renderWorker->device->createTexture(RenderTextureDesc::Texture2D(1, 1, 1, otherTypeFormat, RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS));
        RenderTexture *colorOutput = (type == FramebufferChange::Type::Color) ? changes.pixelTexture.get() : changes.otherTypeTexture.get();
        RenderTexture *depthOutput = (type == FramebufferChange::Type::Depth) ? changes.pixelTexture.get() : changes.otherTypeTexture.get();
#   else
        RenderTexture *colorOutput = changes.pixelTexture.get();
        RenderTexture *depthOutput = changes.pixelTexture.get();
#   endif
        changes.readChangesSet->setTexture(changes.readChangesSet->gOutputChangeColor, colorOutput, RenderTextureLayout::GENERAL);
        changes.readChangesSet->setTexture(changes.readChangesSet->gOutputChangeDepth, depthOutput, RenderTextureLayout::GENERAL);
        changes.readChangesSet->setTexture(changes.readChangesSet->gOutputChangeBoolean, changes.booleanTexture.get(), RenderTextureLayout::GENERAL);

        return changes;
    }

    const FramebufferChange *FramebufferChangePool::get(uint64_t id) const {
        auto it = changesMap.find(id);
        if (it != changesMap.end()) {
            return &it->second;
        }
        else {
            return nullptr;
        }
    }

    void FramebufferChangePool::release(uint64_t id) {
        auto it = changesMap.find(id);
        assert(it != changesMap.end());
        it->second.used = false;
    }
};