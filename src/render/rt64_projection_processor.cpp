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

#if RT64_SLERP_CAMERA_ROTATION
    // A view between two game frames' views, its axes turned from the one's to the other's at a constant rate (a
    // slerp of their rotations, their scales lerped), and its eye on the line between the two eyes (the current one
    // when the position isn't interpolated), or false when the two can't be decomposed or one is mirrored and the
    // other isn't.
    inline bool slerpView(const RigidBody &rigidBody, const hlslpp::float4x4 &prev, const hlslpp::float4x4 &cur, float weight, interop::float4x4 &view) {
        // The eye, the point a view moves to the origin, is kept apart from the axes and put back after the turn.
        auto eyeOf = [](const hlslpp::float4x4 &m, hlslpp::float3 &eye) {
            eye = -hlslpp::mul(hlslpp::float3(m[3].xyz), hlslpp::inverse(extract3x3(m)));
            return std::isfinite(float(eye.x)) && std::isfinite(float(eye.y)) && std::isfinite(float(eye.z));
        };

        const DecomposedTransform a(prev);
        const DecomposedTransform b(cur);
        hlslpp::float3 prevEye, curEye;
        if (!a.valid || !b.valid || (a.coordinateFlip != b.coordinateFlip) || !eyeOf(prev, prevEye) || !eyeOf(cur, curEye)) {
            return false;
        }

        DecomposedTransform lerped = lerpTransforms(a, b, weight, false, false, rigidBody.lerpScale, rigidBody.lerpSkew, rigidBody.lerpPerspective, false);
        float qa[4] = { float(a.rotation.x), float(a.rotation.y), float(a.rotation.z), float(a.rotation.w) };
        float qb[4] = { float(b.rotation.x), float(b.rotation.y), float(b.rotation.z), float(b.rotation.w) };
        float cosTheta = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        if (cosTheta < 0.0f) {
            for (float &q : qb) {
                q = -q;
            }

            cosTheta = -cosTheta;
        }

        // hlslpp's slerp moves along the chord by a ratio of sines, not along the arc, so the weights are taken here.
        float wa = 1.0f - weight;
        float wb = weight;
        if (cosTheta < 0.9999f) {
            const float theta = std::acos(cosTheta);
            const float sinTheta = std::sin(theta);
            wa = std::sin((1.0f - weight) * theta) / sinTheta;
            wb = std::sin(weight * theta) / sinTheta;
        }

        float q[4];
        float length = 0.0f;
        for (int i = 0; i < 4; i++) {
            q[i] = qa[i] * wa + qb[i] * wb;
            length += q[i] * q[i];
        }

        length = std::sqrt(length);
        if (!(length > 0.0f)) {
            return false;
        }

        lerped.rotation = hlslpp::quaternion(q[0] / length, q[1] / length, q[2] / length, q[3] / length);
        view = recomposeMatrix(lerped.rotation, lerped.scale, lerped.skew, hlslpp::float3(0.0f, 0.0f, 0.0f), lerped.perspective);
        const hlslpp::float3 eye = rigidBody.lerpTranslation ? hlslpp::lerp(prevEye, curEye, weight) : curEye;
        const hlslpp::float3 translation = -hlslpp::mul(eye, extract3x3(view));
        view[3][0] = translation.x;
        view[3][1] = translation.y;
        view[3][2] = translation.z;
        return true;
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
    // cut, RT64_SKIP_CAMERA_CUTS) leaves its writes the game's, and so does a group whose writes didn't move together
    // (below), which is no motion of one thing between two frames. Returns whether any write was paired, so the copy is
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

            // The group is paired whole or not at all, and only when its writes moved together: each no further from the
            // group's median motion than an eighth of the view each way, and none further than half the view. A game that
            // draws a strip of quads from a few vertices it writes again and again (Mario Golf's ball tracer) writes a
            // varying number of them in a varying order, so two frames' groups of one size can be other quads, and a pair
            // of the wrong ones drawn between stretched a quad across the picture; and a pair left out of a group that
            // moved by about the limit left that quad's other corners drawn between, a quad as long as half the view.
            const interop::RSPViewport &viewport = curData.rspViewports[t];
            const float maxDx = std::abs(float(viewport.scale.x)) * 4.0f;
            const float maxDy = std::abs(float(viewport.scale.y)) * 4.0f;
            auto unpack = [](uint32_t value, float &x, float &y) {
                x = float(int16_t((value >> 16) & 0xFFFF));
                y = float(int16_t(value & 0xFFFF));
            };

            thread_local std::vector<float> moveX;
            thread_local std::vector<float> moveY;
            moveX.resize(curGroup.size());
            moveY.resize(curGroup.size());
            bool together = true;
            for (size_t k = 0; (k < curGroup.size()) && together; k++) {
                float curX, curY, prevX, prevY;
                unpack(curData.modifyPosUints[curGroup[k] + 1], curX, curY);
                unpack(prevData.modifyPosUints[(*prevGroup)[k] + 1], prevX, prevY);
                moveX[k] = curX - prevX;
                moveY[k] = curY - prevY;
                together = (std::abs(moveX[k]) <= maxDx) && (std::abs(moveY[k]) <= maxDy);
            }

            if (together && (curGroup.size() > 1)) {
                // A group is a few writes, so an insertion sort finds the middle one.
                thread_local std::vector<float> sorted;
                auto median = [](const std::vector<float> &values) {
                    sorted = values;
                    for (size_t i = 1; i < sorted.size(); i++) {
                        const float v = sorted[i];
                        size_t j = i;
                        for (; (j > 0) && (sorted[j - 1] > v); j--) {
                            sorted[j] = sorted[j - 1];
                        }
                        sorted[j] = v;
                    }

                    return sorted[sorted.size() / 2];
                };

                const float medianX = median(moveX);
                const float medianY = median(moveY);
                for (size_t k = 0; (k < curGroup.size()) && together; k++) {
                    together = (std::abs(moveX[k] - medianX) <= (maxDx / 4.0f)) && (std::abs(moveY[k] - medianY) <= (maxDy / 4.0f));
                }
            }

            if (!together) {
                continue;
            }

            auto quarters = [](float v) {
                int32_t rounded = int32_t(v + ((v >= 0.0f) ? 0.5f : -0.5f));
                rounded = (rounded < -32768) ? -32768 : ((rounded > 32767) ? 32767 : rounded);
                return uint32_t(uint16_t(int16_t(rounded)));
            };

            for (size_t k = 0; k < curGroup.size(); k++) {
                const size_t m = curGroup[k];
                float prevX, prevY;
                unpack(prevData.modifyPosUints[(*prevGroup)[k] + 1], prevX, prevY);
                const float x = prevX + moveX[k] * p.curFrameWeight;
                const float y = prevY + moveY[k] * p.curFrameWeight;
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
#           if RT64_SLERP_CAMERA_ROTATION
                // Without a matrix group that decomposes the view, RigidBody::lerp lerps its rotation a component at a time,
                // which shrinks what the two rotations have apart by the cosine of half the angle between them in the frames
                // between: a camera turning while it looks up or down draws the world shifted and squeezed toward its up axis
                // there and lets go of it at each game frame, a judder at the game's rate that grows with the square of the
                // turn (Turok 2, which draws with no matrix group). The view is turned at a constant rate between the two.
                if (!rigidBody->lerpDecompose && rigidBody->lerpRotation) {
                    interop::float4x4 slerped;
                    if (slerpView(*rigidBody, *prevViewMatrix, curViewTransform, p.curFrameWeight, slerped)) {
                        viewMatrix = slerped;
                    }

                    if (slerpView(*rigidBody, *prevViewMatrix, curViewTransform, p.prevFrameWeight, slerped)) {
                        prevViewTransform = slerped;
                    }
                }
#           endif
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