//
// RT64
//

#pragma once

#include "rt64_framebuffer_manager.h"
#include "rt64_projection.h"

namespace RT64 {
    struct FramebufferPair {
        enum class FlushReason {
            None,
            SamplingFromColorImage,
            SamplingFromDepthImage,
            ColorImageChanged,
            DepthImageChanged,
            ProcessDisplayListsEnd
        };

        struct {
            uint32_t address = 0;
            uint8_t fmt = 0;
            uint8_t siz = 0;
            uint16_t width = 0;
            bool formatChanged = false;
        } colorImage;

        struct {
            uint32_t address = 0;
            bool formatChanged = false;
        } depthImage;

        struct {
            bool clearDepthOnly : 1;
        } fastPaths;

        uint32_t displayListAddress;
        uint64_t displayListCounter;
        std::vector<Projection> projections;
        uint32_t projectionCount;
        uint32_t projectionStart;
        uint32_t gameCallCount;
        bool depthRead;
        bool depthWrite;
        bool syncRequired;
        bool fillRectOnly; // This only applies if there's at least one draw call.
        std::vector<uint32_t> startFbDiscards;
        std::vector<FramebufferOperation> startFbOperations;
        std::vector<FramebufferOperation> endFbOperations;
        std::array<uint32_t, 4> ditherPatterns;
        FixedRect scissorRect;
        FixedRect drawColorRect;
        FixedRect viewportRect; // The viewports the pass's triangles were drawn through, each cut to its scissor.
        FixedRect drawDepthRect;
        FlushReason flushReason;
#   if defined(__EMSCRIPTEN__)
        // The rects drawn into the color image, the newest few on their own and the box of the older ones, so a load
        // ends the pair only if it reads pixels drawn in it (RDP::checkImageOverlap). drawColorRect is the box of all.
        static constexpr uint32_t RecentColorRectCount = 8;
        std::array<FixedRect, RecentColorRectCount> recentColorRects;
        uint32_t recentColorRectCursor;
        FixedRect olderColorRect;
#   endif

        void reset();
        void mergeDrawColorRect(const FixedRect &rect);
        void addGameCall(const GameCall &gameCall);
        bool inProjection(uint32_t transformsIndex, Projection::Type type) const;
        int changeProjection(uint32_t transformsIndex, Projection::Type type);
        bool isEmpty() const;
        bool earlyPresentCandidate() const;
    };
};