//
// RT64
//

#include "rt64_framebuffer_pair.h"

#include "shared/rt64_blender.h"

namespace RT64 {
#if defined(__EMSCRIPTEN__)
    // FramebufferPair::DrawnRect

    void FramebufferPair::DrawnRect::merge(const FixedRect &rect) {
        FixedRect::merge(rect);

        FixedRect &recentRect = recent[recentCursor];
        if (!recentRect.isNull()) {
            older.merge(recentRect);
        }

        recentRect = rect;
        recentCursor = (recentCursor + 1) % RecentCount;
    }

    void FramebufferPair::DrawnRect::reset() {
        FixedRect::reset();
        recent.fill(FixedRect());
        recentCursor = 0;
        older.reset();
    }
#endif

    // FramebufferPair

    void FramebufferPair::reset() {
        fastPaths = {};
        flushReason = FlushReason::None;
        displayListAddress = 0;
        displayListCounter = 0;
        projectionCount = 0;
        projectionStart = 0;
        gameCallCount = 0;
        depthRead = false;
        depthWrite = false;
        syncRequired = false;
        fillRectOnly = true;
        ditherPatterns.fill(0);
        scissorRect.reset();
        startFbDiscards.clear();
        startFbOperations.clear();
        endFbOperations.clear();
        drawColorRect.reset();
        viewportRect.reset();
        drawDepthRect.reset();
    }

    void FramebufferPair::addGameCall(const GameCall &gameCall) {
        assert(projectionCount > 0);
        auto &proj = projections[projectionCount - 1];
        proj.addGameCall(gameCall);
        depthRead = depthRead || gameCall.callDesc.otherMode.zCmp();
        depthWrite = depthWrite || gameCall.callDesc.otherMode.zUpd();
        fillRectOnly = fillRectOnly && (proj.type == Projection::Type::Rectangle) && (gameCall.callDesc.otherMode.cycleType() == G_CYC_FILL);
        gameCallCount++;

        // Track what type of color dither this call used. Fill and copy cycles never dither on hardware, whatever the
        // other mode's dither bits say.
        const uint32_t cycleType = gameCall.callDesc.otherMode.cycleType();
        const bool ditherable = (cycleType != G_CYC_FILL) && (cycleType != G_CYC_COPY);
        const uint32_t ditherBits = ditherable ? gameCall.callDesc.otherMode.rgbDither() : G_CD_DISABLE;
        uint32_t ditherIndex = (ditherBits >> G_MDSFT_RGBDITHER) & 0x3;
        ditherPatterns[ditherIndex]++;
    }

    void FramebufferPair::mergeScissor(const FixedRect &rect) {
#   if RT64_SCISSOR_INSIDE_FRAMEBUFFER
        // RT64_SCISSOR_INSIDE_FRAMEBUFFER: scissorRect is the area the pair drew in, which the projection processor and
        // the framebuffer renderer compare a projection's or a rect's own against to tell what covers the frame's whole
        // width: a 3D view or a 2D projection whose aspect RT64 picks (G_EX_ASPECT_AUTO) is kept at its aspect, and a
        // fill of the whole screen is widened to the window's edges. An extended scissor aligned to the centre that
        // reaches past 4:3 (gEXSetScissor with G_EX_ORIGIN_CENTER and offsets, which a widescreen patch draws a screen
        // wipe's colour out to the window's edges under) is far past the framebuffer's left and right edges in the
        // framebuffer's own coordinates, so on each frame that drew under one nothing else covered the whole width:
        // a board's sprites, its HUD and a title card among them, were drawn stretched to the window's width for that
        // frame. What is past the framebuffer's edges counts only to them.
        FixedRect inside = rect;
        inside.ulx = std::max(inside.ulx, 0);
        inside.lrx = std::min(inside.lrx, int32_t(colorImage.width) << 2);
        if (!inside.isNull()) {
            scissorRect.merge(inside);
        }
#   else
        scissorRect.merge(rect);
#   endif
    }

    bool FramebufferPair::inProjection(uint32_t transformsIndex, Projection::Type type) const {
        if (projectionCount > 0) {
            const Projection &lastProj = projections[projectionCount - 1];
            if ((lastProj.transformsIndex == transformsIndex) && (lastProj.type == type)) {
                return true;
            }
        }

        return false;
    }

    int FramebufferPair::changeProjection(uint32_t transformsIndex, Projection::Type type) {
        adjustVector(projections, ++projectionCount);
        const uint32_t projectionIndex = projectionCount - 1;
        auto &projection = projections[projectionIndex];
        projection.reset();
        projection.transformsIndex = transformsIndex;
        projection.type = type;
        return projectionIndex;
    }

    bool FramebufferPair::isEmpty() const {
        return (gameCallCount == 0) && startFbOperations.empty() && endFbOperations.empty();
    }
    
    bool FramebufferPair::earlyPresentCandidate() const {
        // Some games might use screen framebuffers as temporary output for some operations that are clearly
        // not intended to be presented to the user. This function includes some cases commonly found in games.
        const bool singleGameCall = (gameCallCount == 1);
        
        if (singleGameCall) {
            for (uint32_t p = 0; p < projectionCount; p++) {
                const Projection &proj = projections[p];
                if (proj.gameCallCount == 0) {
                    continue;
                }

                const GameCall &call = proj.gameCalls[0];
                if (proj.type == Projection::Type::Rectangle) {
                    // The rect call covers the entire dimensions of the scissor.
                    bool fullScreenRect = call.callDesc.scissorRect.fullyInside(call.callDesc.rect);

                    // VISCVG: Some games will dump coverage by using a special blender that basically outputs coverage only.
                    // Coverage can be useful for emulating the VI anti-aliasing on the CPU, which is the case on the Zelda games
                    // or games with the same engine (e.g. Animal Forest).
                    // 
                    // It is very unlikely this will get presented to the user, so if the only relevant operation in the
                    // framebuffer pair is a rect operation that does this, it won't be valid for early presentation.
                    if (interop::Blender::usesVisualizeCoverageCycle(call.callDesc.otherMode)) {
                        return false;
                    }
                }
            }
        }

        return true;
    }
};