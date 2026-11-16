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
#   if defined(__EMSCRIPTEN__)
        // The box of the rects drawn into the color image, which also keeps the newest few apart and the box of the
        // older ones, so a load ends the pair only if it reads pixels drawn in it (RDP::checkImageOverlap). Its merge()
        // and reset() hide FixedRect's, so the code that draws merges into it as upstream's does.
        struct DrawnRect : FixedRect {
            static constexpr uint32_t RecentCount = 8;
            std::array<FixedRect, RecentCount> recent;
            uint32_t recentCursor = 0;
            FixedRect older;

            void merge(const FixedRect &rect);
            void reset();
        };

        DrawnRect drawColorRect;
#   else
        FixedRect drawColorRect;
#   endif
        FixedRect viewportRect; // The viewports the pass's triangles were drawn through, each cut to its scissor.
        FixedRect drawDepthRect;
        FlushReason flushReason;

        void reset();
        void addGameCall(const GameCall &gameCall);
        void mergeScissor(const FixedRect &rect);
        bool inProjection(uint32_t transformsIndex, Projection::Type type) const;
        int changeProjection(uint32_t transformsIndex, Projection::Type type);
        bool isEmpty() const;
        bool earlyPresentCandidate() const;
    };
};