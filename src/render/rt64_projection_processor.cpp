//
// RT64
//

#include "rt64_projection_processor.h"

#include "common/rt64_math.h"
#include "hle/rt64_workload_queue.h"

namespace RT64 {
#if RT64_INTERPOLATE_CAMERA_EYE
    // The eye of a view matrix, the point it moves to the origin, or false for a view without one (a zero matrix).
    inline bool viewEye(const hlslpp::float4x4 &view, hlslpp::float3 &eye) {
        eye = -hlslpp::mul(hlslpp::float3(view[3].xyz), hlslpp::inverse(extract3x3(view)));
        return std::isfinite(float(eye.x)) && std::isfinite(float(eye.y)) && std::isfinite(float(eye.z));
    }

    // Puts a view's eye at a point, keeping its axes.
    inline void setViewEye(interop::float4x4 &view, const hlslpp::float3 &eye) {
        const hlslpp::float3 translation = -hlslpp::mul(eye, extract3x3(view));
        view[3][0] = translation.x;
        view[3][1] = translation.y;
        view[3][2] = translation.z;
    }
#endif

#if RT64_INTERPOLATE_SCREEN_VERTICES
    // A screen position a game writes into a vertex itself (G_MODIFYVTX's XYSCREEN) is the game frame's own, and
    // RSPModifyCS puts it over the interpolated one in every frame drawn between the game's: what a game projects
    // itself and writes where it lands (Mario Golf's pin flagstick and its flag) stood still a game frame at a time
    // while the course around it, drawn between, moved on every frame, and shook against it as the camera moved, the
    // more the more frames the display draws a game frame (four at 120 Hz to a game of 30). Each write is paired with
    // one the frame before made, in the order the game wrote them, among the writes to vertices of one projection: the
    // one process() interpolates its camera from (its viewProjections map) where that one has as many, or else the
    // frame before's in the same place among the projections written through, where both frames wrote through as many
    // and that one as many. A game loads the same camera more than once a frame and in a varying number, so the map
    // can pair a projection with an equal one the writes didn't go through (Mario Golf's flagstick writes, through its
    // sixth projection, which the map paired with the frame before's third, of none). Each pair is drawn between its
    // two by the frame's weight, in the quarter pixels the RSP takes. A projection the map pairs with none (a camera
    // cut, RT64_SKIP_CAMERA_CUTS) leaves its writes the game's, and so does a pair further apart than half the view,
    // which is no motion of one thing between two frames. Returns whether any write was paired, so the copy is
    // uploaded in every present of the frame, its last (weight 1) included, which puts the game's own positions back
    // over the frames between.
    bool ProjectionProcessor::interpolateScreenWrites(const ProcessParams &p, uint32_t w, std::vector<uint32_t> &modifyPosUints) {
        const Workload &workload = p.workloadQueue->workloads[w];
        const GameFrameMap::WorkloadMap &workloadMap = p.curFrame->frameMap.workloads[w];
        if ((p.prevFrame == nullptr) || !workloadMap.mapped || workload.debuggerCamera.enabled) {
            return false;
        }

        const Workload &prevWorkload = p.workloadQueue->workloads[workloadMap.prevWorkloadIndex];
        const DrawData &curData = workload.drawData;
        const DrawData &prevData = prevWorkload.drawData;

        // Each projection's XYSCREEN writes, by their place in modifyPosUints, in the order the game made them, the
        // projections in the order the game first wrote through each.
        typedef std::vector<std::pair<uint32_t, std::vector<size_t>>> ScreenWrites;
        auto collect = [](const DrawData &drawData, ScreenWrites &writes) {
            writes.clear();
            for (size_t m = 0; (m + 1) < drawData.modifyPosUints.size(); m += 2) {
                const uint32_t target = drawData.modifyPosUints[m];
                const uint32_t vertexIndex = target >> 1;
                if (((target & 0x1) != 0) || (vertexIndex >= drawData.viewProjIndices.size())) {
                    continue;
                }

                const uint32_t t = drawData.viewProjIndices[vertexIndex];
                size_t g = 0;
                while ((g < writes.size()) && (writes[g].first != t)) {
                    g++;
                }

                if (g == writes.size()) {
                    writes.emplace_back(t, std::vector<size_t>());
                }

                writes[g].second.push_back(m);
            }
        };

        thread_local ScreenWrites curWrites;
        thread_local ScreenWrites prevWrites;
        collect(curData, curWrites);
        if (curWrites.empty()) {
            return false;
        }

        collect(prevData, prevWrites);
        bool pairedAny = false;
        for (size_t g = 0; g < curWrites.size(); g++) {
            const uint32_t t = curWrites[g].first;
            const std::vector<size_t> &curGroup = curWrites[g].second;
            if ((t >= workloadMap.viewProjections.size()) || !workloadMap.viewProjections[t].mapped || (t >= curData.rspViewports.size())) {
                continue;
            }

            const uint32_t prevT = workloadMap.viewProjections[t].prevTransformIndex;
            const std::vector<size_t> *prevGroup = nullptr;
            for (const auto &group : prevWrites) {
                if ((group.first == prevT) && (group.second.size() == curGroup.size())) {
                    prevGroup = &group.second;
                    break;
                }
            }

            if ((prevGroup == nullptr) && (prevWrites.size() == curWrites.size()) && (prevWrites[g].second.size() == curGroup.size())) {
                prevGroup = &prevWrites[g].second;
            }

            if (prevGroup == nullptr) {
                continue;
            }

            const interop::RSPViewport &viewport = curData.rspViewports[t];
            const float maxDx = std::abs(float(viewport.scale.x)) * 4.0f;
            const float maxDy = std::abs(float(viewport.scale.y)) * 4.0f;
            for (size_t k = 0; k < curGroup.size(); k++) {
                const size_t m = curGroup[k];
                const uint32_t curValue = curData.modifyPosUints[m + 1];
                const uint32_t prevValue = prevData.modifyPosUints[(*prevGroup)[k] + 1];
                const float curX = float(int16_t((curValue >> 16) & 0xFFFF));
                const float curY = float(int16_t(curValue & 0xFFFF));
                const float prevX = float(int16_t((prevValue >> 16) & 0xFFFF));
                const float prevY = float(int16_t(prevValue & 0xFFFF));
                if ((std::abs(curX - prevX) > maxDx) || (std::abs(curY - prevY) > maxDy)) {
                    continue;
                }

                auto quarters = [](float v) {
                    int32_t rounded = int32_t(v + ((v >= 0.0f) ? 0.5f : -0.5f));
                    rounded = (rounded < -32768) ? -32768 : ((rounded > 32767) ? 32767 : rounded);
                    return uint32_t(uint16_t(int16_t(rounded)));
                };

                const float x = prevX + (curX - prevX) * p.curFrameWeight;
                const float y = prevY + (curY - prevY) * p.curFrameWeight;
                modifyPosUints[m + 1] = (quarters(x) << 16) | quarters(y);
                pairedAny = true;
            }
        }

        return pairedAny;
    }
#endif

    inline void adjustProjectionMatrix(interop::float4x4 &matrix, const float aspectRatioScale) {
        matrix[0][0] *= aspectRatioScale;
        matrix[1][0] *= aspectRatioScale;
        matrix[2][0] *= aspectRatioScale;
        matrix[3][0] *= aspectRatioScale;
    }
    
    // ProjectionProcessor

    ProjectionProcessor::ProjectionProcessor() { }

    ProjectionProcessor::~ProjectionProcessor() {
        bufferUploader.reset(nullptr);
    }

    void ProjectionProcessor::setup(RenderWorker *worker) {
        bufferUploader = std::make_unique<BufferUploader>(worker->device);
    }

    void ProjectionProcessor::process(const ProcessParams &p) {
        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            DrawData &drawData = workload.drawData;

            // Copy the data.
            drawData.modViewTransforms = drawData.viewTransforms;
            drawData.modProjTransforms = drawData.projTransforms;
            drawData.modViewProjTransforms = drawData.viewProjTransforms;
            drawData.prevViewTransforms = drawData.viewTransforms;
            drawData.prevProjTransforms = drawData.projTransforms;
            drawData.prevViewProjTransforms = drawData.viewProjTransforms;
        }

        for (size_t s = 0; s < p.curFrame->perspectiveScenes.size(); s++) {
            processScene(p, p.curFrame->perspectiveScenes[s], s);
        }

        for (size_t s = 0; s < p.curFrame->orthographicScenes.size(); s++) {
            processScene(p, p.curFrame->orthographicScenes[s], s);
        }
    }

    void ProjectionProcessor::processScene(const ProcessParams &p, const GameScene &scene, size_t sceneIndex) {
        for (size_t i = 0; i < scene.projections.size(); i++) {
            const GameIndices::Projection &sceneProj = scene.projections[i];
            Workload &workload = p.workloadQueue->workloads[sceneProj.workloadIndex];
            DrawData &drawData = workload.drawData;
            const FramebufferPair &fbPair = workload.fbPairs[sceneProj.fbPairIndex];
            const Projection &proj = fbPair.projections[sceneProj.projectionIndex];
            const uint16_t viewportOrigin = drawData.viewportOrigins[proj.transformsIndex];
            assert(proj.transformsIndex > 0);

            // Skip projections that didn't actually draw anything.
            if (proj.scissorRect.isNull()) {
                continue;
            }

            // Check the current mapping for the projection.
            const interop::float4x4 *prevProjMatrix = nullptr;
            const interop::float4x4 *prevViewMatrix = nullptr;
            const RigidBody *rigidBody = nullptr;
            const GameFrameMap::WorkloadMap &workloadMap = p.curFrame->frameMap.workloads[sceneProj.workloadIndex];
            if ((p.prevFrame != nullptr) && workloadMap.mapped && !workload.debuggerCamera.enabled) {
                const GameFrameMap::ViewProjectionMap &viewProjMap = workloadMap.viewProjections[proj.transformsIndex];
                if (viewProjMap.mapped) {
                    const Workload &prevWorkload = p.workloadQueue->workloads[workloadMap.prevWorkloadIndex];
                    prevViewMatrix = &prevWorkload.drawData.viewTransforms[viewProjMap.prevTransformIndex];
                    prevProjMatrix = &prevWorkload.drawData.projTransforms[viewProjMap.prevTransformIndex];
                    rigidBody = &viewProjMap.rigidBody;
                }
            }

            const uint32_t curProjGroupIndex = workload.drawData.viewProjTransformGroups[proj.transformsIndex];
            const TransformGroup &curProjGroup = workload.drawData.transformGroups[curProjGroupIndex];
            bool adjustAspectRatio = (curProjGroup.aspectMode == G_EX_ASPECT_ADJUST);
            if (curProjGroup.aspectMode == G_EX_ASPECT_AUTO) {
                FixedRect intersectionRect = proj.scissorRect;
                if (proj.usesViewport()) {
                    const interop::RSPViewport &viewport = drawData.rspViewports[proj.transformsIndex];
                    const int16_t *viewportClipRatios = &drawData.viewportClipRatios[proj.transformsIndex * 4];
                    intersectionRect = intersectionRect.intersection(viewport.rect(viewportClipRatios));
                }

                if (!intersectionRect.isEmpty()) {
                    bool coversWholeWidth = (intersectionRect.ulx <= fbPair.scissorRect.ulx) && (intersectionRect.lrx >= fbPair.scissorRect.lrx);
                    bool horizontalRatio = (intersectionRect.width(true, true) > intersectionRect.height(true, true));
                    adjustAspectRatio = (viewportOrigin == G_EX_ORIGIN_NONE) && coversWholeWidth && horizontalRatio;
                }
            }
 
            float projRatioScale = adjustAspectRatio ? (1.0f / p.aspectRatioScale) : 1.0f;
            interop::float4x4 &viewMatrix = drawData.modViewTransforms[proj.transformsIndex];
            interop::float4x4 &projMatrix = drawData.modProjTransforms[proj.transformsIndex];
            interop::float4x4 &viewProjMatrix = drawData.modViewProjTransforms[proj.transformsIndex];
            viewMatrix = drawData.viewTransforms[proj.transformsIndex];
            projMatrix = drawData.projTransforms[proj.transformsIndex];
            viewProjMatrix = drawData.viewProjTransforms[proj.transformsIndex];

            // Debugger camera.
            if (workload.debuggerCamera.enabled && (proj.type == Projection::Type::Perspective) && (workload.debuggerCamera.sceneIndex == sceneIndex)) {
                viewMatrix = workload.debuggerCamera.viewMatrix;
                projMatrix = workload.debuggerCamera.projMatrix;
            }

            adjustProjectionMatrix(projMatrix, projRatioScale);

            interop::float4x4 &prevViewTransform = drawData.prevViewTransforms[proj.transformsIndex];
            interop::float4x4 &prevProjTransform = drawData.prevProjTransforms[proj.transformsIndex];
            if ((prevProjMatrix != nullptr) && (prevViewMatrix != nullptr) && (rigidBody != nullptr)) {
                const interop::float4x4 curViewTransform = viewMatrix;
                const interop::float4x4 curProjTransform = projMatrix;
                interop::float4x4 adjustedPrevProj = *prevProjMatrix;
                adjustProjectionMatrix(adjustedPrevProj, projRatioScale);
                viewMatrix = rigidBody->lerp(p.curFrameWeight, *prevViewMatrix, curViewTransform, true);
                prevViewTransform = rigidBody->lerp(p.prevFrameWeight, *prevViewMatrix, curViewTransform, true);

                // We only interpolate the projection if the view matrix has been interpolated.
                const bool interpolateProjection = rigidBody->lerpTranslation || rigidBody->lerpRotation;
#           if RT64_INTERPOLATE_CAMERA_EYE
                // The view's translation is the eye in the view's own axes, so lerping it took the eye of a camera far from the
                // world's origin off the line between its two places as the camera turned, and what is near the camera swung
                // in the frames between. The eye is lerped instead (kept at the current one when the position isn't), and
                // each interpolated view's translation is put back from it.
                hlslpp::float3 prevEye, curEye;
                if (viewEye(*prevViewMatrix, prevEye) && viewEye(curViewTransform, curEye)) {
                    setViewEye(viewMatrix, rigidBody->lerpTranslation ? hlslpp::lerp(prevEye, curEye, p.curFrameWeight) : curEye);
                    setViewEye(prevViewTransform, rigidBody->lerpTranslation ? hlslpp::lerp(prevEye, curEye, p.prevFrameWeight) : curEye);
                }
#           endif

                if (interpolateProjection) {
                    projMatrix = lerpMatrix(adjustedPrevProj, curProjTransform, p.curFrameWeight);
                    prevProjTransform = lerpMatrix(adjustedPrevProj, curProjTransform, p.prevFrameWeight);
                }
                else {
                    projMatrix = curProjTransform;
                    prevProjTransform = curProjTransform;
                }
            }
            else {
                prevViewTransform = viewMatrix;
                prevProjTransform = projMatrix;
            }

            viewProjMatrix = hlslpp::mul(viewMatrix, projMatrix);

            interop::float4x4 &prevViewProjTransform = drawData.prevViewProjTransforms[proj.transformsIndex];
            prevViewProjTransform = hlslpp::mul(prevViewTransform, prevProjTransform);
        }
    }

    void ProjectionProcessor::upload(const ProcessParams &p) {
        uploads.clear();
#   if RT64_EXPAND_SCREEN_VERTICES || RT64_INTERPOLATE_SCREEN_VERTICES
        // RT64_EXPAND_SCREEN_VERTICES: a screen position a game writes into a vertex itself (G_MODIFYVTX's XYSCREEN)
        // never passes through the projection, so the x scale process() gave the projections for a picture wider than
        // 4:3 missed it, and the wide viewport then stretched it away from the picture's middle: what a game projects
        // itself and writes where it lands (Mario Golf's pin flagstick) stood away from what the projection placed (the
        // hole) and from the 2D the game put at the same spot (the pin's marker). Each written x is scaled toward its
        // viewport's centre by its vertex's projection's scale, as its projected vertices are, and the scaled copy is
        // uploaded over what the workload uploaded only where one was scaled. With RT64_INTERPOLATE_SCREEN_VERTICES
        // each write is drawn between the frame before's and its own first (interpolateScreenWrites()), and that is
        // what is scaled.
        //
        // The scale is read from the projection process() made, not noted as it was made, since games' own changes to
        // RT64 rewrite those lines: process() scales only the projection's x column, and interpolates it whole between
        // two frames' projections, which a zoom changes in x and y alike, so the length of its x column over its y
        // column's is the game's times the scale (their first three rows, which a view the game folds into its
        // projection only turns).
        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            const DrawData &drawData = workload.drawData;
            if (drawData.modifyPosUints.empty()) {
                continue;
            }

            if (screenModifyPosUints.size() <= w) {
                screenModifyPosUints.resize(w + 1);
            }

            std::vector<uint32_t> &modifyPosUints = screenModifyPosUints[w];
            modifyPosUints = drawData.modifyPosUints;
            bool changedAny = false;
#       if RT64_INTERPOLATE_SCREEN_VERTICES
            changedAny = interpolateScreenWrites(p, w, modifyPosUints);
#       endif
#       if RT64_EXPAND_SCREEN_VERTICES
            for (size_t m = 0; (m + 1) < modifyPosUints.size(); m += 2) {
                const uint32_t target = modifyPosUints[m];
                if ((target & 0x1) != 0) {
                    // A ZSCREEN write.
                    continue;
                }

                const uint32_t vertexIndex = target >> 1;
                if (vertexIndex >= drawData.viewProjIndices.size()) {
                    continue;
                }

                const uint32_t t = drawData.viewProjIndices[vertexIndex];
                if ((t >= drawData.projTransforms.size()) || (t >= drawData.modProjTransforms.size()) || (t >= drawData.rspViewports.size())) {
                    continue;
                }

                const interop::float4x4 &gameProj = drawData.projTransforms[t];
                const interop::float4x4 &madeProj = drawData.modProjTransforms[t];
                const float gameX = float(hlslpp::length(hlslpp::float3(gameProj[0][0], gameProj[1][0], gameProj[2][0])));
                const float gameY = float(hlslpp::length(hlslpp::float3(gameProj[0][1], gameProj[1][1], gameProj[2][1])));
                const float madeX = float(hlslpp::length(hlslpp::float3(madeProj[0][0], madeProj[1][0], madeProj[2][0])));
                const float madeY = float(hlslpp::length(hlslpp::float3(madeProj[0][1], madeProj[1][1], madeProj[2][1])));
                if ((gameX <= 0.0f) || (gameY <= 0.0f) || (madeY <= 0.0f)) {
                    continue;
                }

                const float ratioScale = (madeX / madeY) / (gameX / gameY);
                if (!(ratioScale > 0.0f) || ((ratioScale > 0.9999f) && (ratioScale < 1.0001f))) {
                    continue;
                }

                // Quarter pixels, as the RSP takes them, x in the high half.
                const uint32_t value = modifyPosUints[m + 1];
                const float centerX = float(drawData.rspViewports[t].translate.x) * 4.0f;
                const float screenX = float(int16_t((value >> 16) & 0xFFFF));
                const float scaledX = centerX + (screenX - centerX) * ratioScale;
                int32_t roundedX = int32_t(scaledX + ((scaledX >= 0.0f) ? 0.5f : -0.5f));
                roundedX = (roundedX < -32768) ? -32768 : ((roundedX > 32767) ? 32767 : roundedX);
                modifyPosUints[m + 1] = (uint32_t(uint16_t(int16_t(roundedX))) << 16) | (value & 0xFFFF);
                changedAny = true;
            }
#       endif

            if (changedAny) {
                std::pair<size_t, size_t> modifyRange = { 0, modifyPosUints.size() };
                uploads.emplace_back(BufferUploader::Upload{ modifyPosUints.data(), modifyRange, sizeof(uint32_t), RenderBufferFlag::FORMATTED, { RenderFormat::R32_UINT }, &workload.drawBuffers.modifyPosUintsBuffer });
            }
        }
#   endif

        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            const DrawData &drawData = workload.drawData;
            DrawBuffers &drawBuffers = workload.drawBuffers;
            std::pair<size_t, size_t> uploadRange = { 0, drawData.viewProjTransforms.size() };
            uploads.emplace_back(BufferUploader::Upload{ drawData.modViewProjTransforms.data(), uploadRange, sizeof(interop::float4x4), RenderBufferFlag::STORAGE, { }, &drawBuffers.viewProjTransformsBuffer });
        }

        bufferUploader->submit(p.worker, uploads);
    }
};