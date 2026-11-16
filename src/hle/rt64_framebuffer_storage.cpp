//
// RT64
//

#include "rt64_framebuffer_storage.h"

#include <cassert>
#include <cstring>

#if defined(__EMSCRIPTEN__)
#   include <algorithm>
#   include <mutex>
#   include <unordered_map>
#endif

namespace RT64 {
#if defined(__EMSCRIPTEN__)
    // The bytes each framebuffer (by address and size) was last stored with, in any workload: a store of the same bytes
    // shares them. Weak, so the bytes go once the last workload holding them is reset.
    static std::mutex sharedBytesMutex;
    static std::unordered_map<uint64_t, std::weak_ptr<const std::vector<uint8_t>>> sharedBytes;
    static size_t sharedBytesPruneAt = 64;
#endif

    // FramebufferStorage

    FramebufferStorage::FramebufferStorage() {
        reset();
    }

    void FramebufferStorage::reset() {
        rdramUsed = 0;
        handleVector.clear();
    }

    void FramebufferStorage::store(uint32_t fbPairIndex, uint32_t address, const uint8_t *data, uint32_t size) {
        Handle handle;
        handle.fbPairIndex = fbPairIndex;
        handle.address = address;
        handle.size = size;
#   if defined(__EMSCRIPTEN__)
        handle.rdramIndex = 0;
        {
            std::scoped_lock<std::mutex> lock(sharedBytesMutex);
            std::weak_ptr<const std::vector<uint8_t>> &last = sharedBytes[(uint64_t(address) << 32) | size];
            handle.bytes = last.lock();
            if ((handle.bytes == nullptr) || (memcmp(handle.bytes->data(), data, size) != 0)) {
                handle.bytes = std::make_shared<const std::vector<uint8_t>>(data, data + size);
                last = handle.bytes;
            }

            // Framebuffers come and go: forget those no workload holds any more once there are many.
            if (sharedBytes.size() >= sharedBytesPruneAt) {
                for (auto it = sharedBytes.begin(); it != sharedBytes.end();) {
                    it = it->second.expired() ? sharedBytes.erase(it) : std::next(it);
                }

                sharedBytesPruneAt = std::max<size_t>(64, sharedBytes.size() * 2);
            }
        }
#   else
        uint32_t dstIndex = rdramUsed;
        rdramUsed += size;
        if (rdramUsed > rdramData.size()) {
            const uint32_t newSize = (rdramUsed * 3) / 2;
            rdramData.resize(newSize, 0);
        }

        memcpy(rdramData.data() + dstIndex, data, size);
        handle.rdramIndex = dstIndex;
#   endif
        handleVector.emplace_back(std::move(handle));
    }

    const FramebufferStorage::Handle *FramebufferStorage::get(uint32_t maxFbPairIndex, uint32_t address) const {
        const FramebufferStorage::Handle *maxHandle = nullptr;
        for (const auto &handle : handleVector) {
            if (handle.fbPairIndex > maxFbPairIndex) {
                continue;
            }

            if (handle.address != address) {
                continue;
            }

            maxHandle = &handle;
        }

        return maxHandle;
    }

    const uint8_t *FramebufferStorage::getRDRAM(const Handle &handle) const {
#   if defined(__EMSCRIPTEN__)
        return handle.bytes->data();
#   else
        assert((handle.rdramIndex + handle.size) <= rdramData.size());
        return rdramData.data() + handle.rdramIndex;
#   endif
    }
};