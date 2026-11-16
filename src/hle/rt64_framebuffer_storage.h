//
// RT64
//

// The framebuffer storage can hold the state of the tracked RAM by all framebuffers
// at the start of a frame. Sometimes it's necessary for the high resolution renderer
// to reload the contents from RDRAM in case the resources get resized and discarded.
// The storage is the resource it can use to fix that.
//
// With Emscripten a framebuffer's bytes that match the ones it last stored, in any workload, are shared with that
// store rather than copied: every RDRAM check stores every framebuffer RT64 knows into the workload it is in, and the
// workload queue a page keeps (RT64_GAME_FRAME_WORKLOADS, 64) held 64 copies of each, each workload's at 1.5 times the
// most it had stored. A game with eight 640x480 framebuffers kept 470 MB of its page's heap in them, and a ninth ran it
// out of memory. A framebuffer whose RAM changed is still a copy of its own in each workload that stores it.

#pragma once

#include "common/rt64_common.h"

#include <map>
#include <memory>

namespace RT64 {
    struct FramebufferStorage {
        struct Handle {
            uint32_t fbPairIndex;
            uint32_t address;
            uint32_t rdramIndex;
            uint32_t size;
#       if defined(__EMSCRIPTEN__)
            // The bytes, shared by every handle (in any workload) that stored the same ones at the same address.
            std::shared_ptr<const std::vector<uint8_t>> bytes;
#       endif
        };

        uint32_t rdramUsed;
        std::vector<uint8_t> rdramData;
        std::vector<Handle> handleVector;

        FramebufferStorage();
        void reset();
        void store(uint32_t fbPairIndex, uint32_t address, const uint8_t *data, uint32_t size);
        const Handle *get(uint32_t maxFbPairIndex, uint32_t address) const;
        const uint8_t *getRDRAM(const Handle &handle) const;
    };
};