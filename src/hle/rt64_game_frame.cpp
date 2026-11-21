//
// RT64
//

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "common/rt64_math.h"

#include "rt64_game_frame.h"
#include "rt64_workload_queue.h"

#include "xxHash/xxh3.h"

namespace RT64 {
    // GameFrame
    
    bool GameFrame::areFramebufferPairsCompatible(const WorkloadQueue &workloadQueue, const GameIndices::FramebufferPair &first, const GameIndices::FramebufferPair &second) {
        if (first == second) {
            return true;
        }

        const Workload &firstWorkload = workloadQueue.workloads[first.workloadIndex];
        const Workload &secondWorkload = workloadQueue.workloads[second.workloadIndex];
        const auto &firstFbPair = firstWorkload.fbPairs[first.fbPairIndex];
        const auto &secondFbPair = secondWorkload.fbPairs[second.fbPairIndex];
        if ((firstFbPair.depthRead || firstFbPair.depthWrite) && (secondFbPair.depthRead || secondFbPair.depthWrite)) {
            if (firstFbPair.depthImage.address != secondFbPair.depthImage.address) {
                return false;
            }
        }

        const auto &firstColorImage = firstFbPair.colorImage;
        const auto &secondColorImage = secondFbPair.colorImage;
        if ((firstColorImage.address != secondColorImage.address) ||
            (firstColorImage.fmt != secondColorImage.fmt) ||
            (firstColorImage.siz != secondColorImage.siz) ||
            (firstColorImage.width != secondColorImage.width))
        {
            return false;
        }

        return true;
    }

    bool GameFrame::isSceneCompatible(const WorkloadQueue &workloadQueue, const GameScene &scene, const GameIndices::Projection &proj) {
        assert(!scene.projections.empty());

        const float MatrixDiffTolerance = 1e-6f;
        const Workload &workload = workloadQueue.workloads[proj.workloadIndex];
        const FramebufferPair &fbPair = workload.fbPairs[proj.fbPairIndex];
        const Projection &fbProj = fbPair.projections[proj.projectionIndex];
        const GameIndices::Projection &firstProj = scene.projections.front();
#if RT64_GAME_FRAME_WORKLOADS
        // A frame can be several workloads, but a scene is matched through its first workload's maps alone (matchScene),
        // so it stays in one workload, as it is when every workload is a frame of its own.
        if (firstProj.workloadIndex != proj.workloadIndex) {
            return false;
        }
#endif
        if (!areFramebufferPairsCompatible(workloadQueue, { firstProj.workloadIndex, firstProj.fbPairIndex }, { proj.workloadIndex, proj.fbPairIndex })) {
            return false;
        }

        const Workload &cmpWorkload = workloadQueue.workloads[firstProj.workloadIndex];
        const FramebufferPair &cmpFbPair = cmpWorkload.fbPairs[firstProj.fbPairIndex];
        const Projection &cmpProj = cmpFbPair.projections[firstProj.projectionIndex];
        const interop::float4x4 &cmpViewMatrix = cmpWorkload.drawData.viewTransforms[cmpProj.transformsIndex];
        const interop::float4x4 &fbViewMatrix = workload.drawData.viewTransforms[fbProj.transformsIndex];
        const float viewMatrixDiff = matrixDifference(cmpViewMatrix, fbViewMatrix);
        if (viewMatrixDiff > MatrixDiffTolerance) {
            return false;
        }

        const interop::float4x4 &cmpProjMatrix = cmpWorkload.drawData.projTransforms[cmpProj.transformsIndex];
        const interop::float4x4 &fbProjMatrix = workload.drawData.projTransforms[fbProj.transformsIndex];
        const float projMatrixDiff = matrixDifference(cmpProjMatrix, fbProjMatrix);
        if (projMatrixDiff > MatrixDiffTolerance) {
            return false;
        }

        return true;
    }

    void GameFrame::set(WorkloadQueue &workloadQueue, const uint32_t *workloadIndices, uint32_t indicesCount) {
        assert(workloadIndices != nullptr);
        assert(indicesCount > 0);

        matched = false;
        perspectiveScenes.clear();
        orthographicScenes.clear();
        workloads.clear();
        workloads.insert(workloads.end(), workloadIndices, workloadIndices + indicesCount);

        auto addProjection = [&](const WorkloadQueue &workloadQueue, const GameIndices::Projection &newProj, std::vector<GameScene> &gameScenes) {
            bool added = false;
            for (auto &gameScene : gameScenes) {
                if (isSceneCompatible(workloadQueue, gameScene, newProj)) {
                    gameScene.projections.emplace_back(newProj);
                    added = true;
                    break;
                }
            }

            if (!added) {
                gameScenes.emplace_back(GameScene());
                gameScenes.back().projections.emplace_back(newProj);
            }
        };

        for (uint32_t i = 0; i < indicesCount; i++) {
            uint32_t w = workloadIndices[i];
            const Workload &workload = workloadQueue.workloads[w];
            for (uint32_t f = 0; f < workload.fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                for (uint32_t p = 0; p < fbPair.projectionCount; p++) {
                    const GameIndices::Projection newProj = { w, f, p };
                    const auto &fbPairProj = fbPair.projections[p];
                    switch (fbPairProj.type) {
                    case Projection::Type::Perspective:
                        addProjection(workloadQueue, newProj, perspectiveScenes);
                        break;
                    case Projection::Type::Orthographic:
                        addProjection(workloadQueue, newProj, orthographicScenes);
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        // Use the default values for the preset scene.
        presetScene = PresetScene();

        /*
        // Use the settings from all enabled presets. Use the last one sorted by name enabled.
        // TODO: Figure out a way to skip the linear lookup on the map.
        for (const auto &it : sceneLibrary.presetMap) {
            if (!it.second.enabled) {
                continue;
            }

            gameFrame.presetScene = it.second;
        }
        */

        /*
        TODO: Must be per projection.
        if (presetScene.estimateAmbientLight && (lightManager.ambientSum > 0)) {
            hlslpp::float3 ambientLight = { 0.01f, 0.01f, 0.01f };
            ambientLight = lightManager.estimatedAmbientLight(gameFrame.presetScene.ambientLightIntensity);
            presetScene.ambientBaseColor = ambientLight;
            presetScene.ambientNoGIColor = ambientLight;
        }
        */

        for (const GameScene &scene : perspectiveScenes) {
            for (const GameIndices::Projection &projection : scene.projections) {
                Workload &workload = workloadQueue.workloads[projection.workloadIndex];
                FramebufferPair &fbPair = workload.fbPairs[projection.fbPairIndex];
                Projection &proj = fbPair.projections[projection.projectionIndex];

                // Add all the lights stored in the workload.
                for (const interop::PointLight &light : workload.pointLights) {
                    proj.addPointLight(light);
                }
            }
        }

        frameMap.clear();
        frameMap.workloads.resize(workloadQueue.workloads.size());
    }
    
    typedef std::pair<uint32_t, uint32_t> IndexPair;

    bool operator<(const IndexPair &lhs, const IndexPair &rhs) {
        return (lhs.first < rhs.first) || ((lhs.first == rhs.first) && lhs.second < rhs.second);
    }

    // Sorts the pairs in the order operator< gives and drops the repeats. Each pair becomes a 64-bit key that sorts the same way,
    // and the keys are sorted a byte at a time, skipping the bytes no key uses: the indices are small, so that is a few passes.
    static void sortUniqueIndexPairs(std::vector<IndexPair> &pairs) {
        thread_local std::vector<uint64_t> keys;
        thread_local std::vector<uint64_t> sortedKeys;
        const size_t count = pairs.size();
        keys.resize(count);
        sortedKeys.resize(count);
        uint64_t keyBits = 0;
        for (size_t i = 0; i < count; i++) {
            keys[i] = (uint64_t(pairs[i].first) << 32) | pairs[i].second;
            keyBits |= keys[i];
        }

        for (uint32_t shift = 0; shift < 64; shift += 8) {
            // A byte that is zero in every key leaves the order as it is.
            if (((keyBits >> shift) & 0xFFU) == 0) {
                continue;
            }

            uint32_t offsets[256] = {};
            for (size_t i = 0; i < count; i++) {
                offsets[(keys[i] >> shift) & 0xFFU]++;
            }

            uint32_t offset = 0;
            for (uint32_t &o : offsets) {
                const uint32_t bucketCount = o;
                o = offset;
                offset += bucketCount;
            }

            for (size_t i = 0; i < count; i++) {
                sortedKeys[offsets[(keys[i] >> shift) & 0xFFU]++] = keys[i];
            }

            keys.swap(sortedKeys);
        }

        size_t uniqueCount = 0;
        for (size_t i = 0; i < count; i++) {
            if ((i == 0) || (keys[i] != keys[i - 1])) {
                pairs[uniqueCount++] = IndexPair(uint32_t(keys[i] >> 32), uint32_t(keys[i]));
            }
        }

        pairs.resize(uniqueCount);
    }

    struct MatchCandidate {
        uint32_t curIndex = 0;
        uint32_t prevIndex = 0;
        float difference = FLT_MAX;

        MatchCandidate(uint32_t curIndex, uint32_t prevIndex, float difference) {
            this->curIndex = curIndex;
            this->prevIndex = prevIndex;
            this->difference = difference;
        }
    };

    bool operator<(const MatchCandidate &lhs, const MatchCandidate &rhs) {
        return lhs.difference < rhs.difference;
    }

    struct TransformMatchResult {
        float positionDifference = FLT_MAX;
        float orientationDifference = FLT_MAX;
        float screenSpaceDifference = FLT_MAX;
        float scaleDifference = FLT_MAX;
        bool valid = false;

        float computeDifference() const {
            if (!valid) {
                return FLT_MAX;
            }

            float totalDiff = 0.0f;

            const float PositionDiffScale = 1.0f;
            if (positionDifference < FLT_MAX) {
                totalDiff += positionDifference * PositionDiffScale;
            }

            const float OrientationDiffScale = 1.0f;
            if (orientationDifference < FLT_MAX) {
                totalDiff += orientationDifference * OrientationDiffScale;
            }

            const float ScreenSpaceDiffScale = 1.0f;
            if (screenSpaceDifference < FLT_MAX) {
                totalDiff += screenSpaceDifference * ScreenSpaceDiffScale;
            }

            const float ScaleDiffScale = 1.0f;
            if (scaleDifference < FLT_MAX) {
                totalDiff += scaleDifference * ScaleDiffScale;
            }

            return totalDiff;
        }
    };

#if RT64_SKIP_CAMERA_CUTS
    // Whether a perspective camera cut from one shot to another between two game frames, rather than moved: it turned
    // more than 45 degrees in one game frame, or its eye jumped more than 8 of its near plane's distances and more than
    // 4 times as far as it moved the frame before, or it held still the frame before (its eye moved less than a
    // twentieth of a near plane distance) and now turned more than 20 degrees or jumped more than 4. The eye's step is
    // kept for the next frame's check, a cut's jump too, as prevEyeStep, -1 when the frame before's isn't known. A cut
    // once left it -1, so a camera that went on moving over 8 near plane distances a game frame after a cut was called
    // a cut again on every frame until it slowed, and drawn without the frames between: Mario Golf's flyovers of
    // Yoshi's Island's holes 14 and 17 fly low and fast from the doors that open on them, and stepped at the game's 30
    // a second for one to two seconds.
    //
    // A game that draws its frames with no matrix groups of its own pairs the camera of each frame with the one before,
    // and RT64's frames between drew the cut halfway: Mario Golf cuts from the golfer to the ball and from the ball to
    // where it lands, its eye 8-760 near plane distances away and often turned more than 90 degrees, and the frames
    // between drew its trees stretched to several times their height for a frame. Its cameras that move (its flyovers,
    // the ball's flight) moved at most about 7 near plane distances and turned at most about 24 degrees a game frame, and
    // its fastest moved as fast for many frames running, which the second check's ratio to the frame before leaves out.
    // Its end credits cut from one golfer's shot to the next, each shot a camera that holds still, by turning it 30-43
    // degrees and moving its eye 1-7 near plane distances, under both of those, and the frames between drew the next
    // golfer partway from one camera to the other: a camera that moves from rest by that much in one game frame cut.
    //
    // The turn is the whole turn from one camera to the other, the angle of the rotation between their axes, and not
    // only how far the way the camera looks moved: San Francisco Rush 2049's attract race cuts between cameras that
    // look straight down at the cars by rolling about the way they look, turning the picture 86-180 degrees, which left
    // the way they look where it was.
    //
    // A step of -1 is one that isn't known (measureCameraMove(), worldEyeStep()), and only the turn is judged.
    static bool isCameraCut(float turnCosine, float step, float prevEyeStep) {
        const float MaxTurnCosine = 0.7071f;
        const float MinJump = 8.0f;
        const float MinJumpRatio = 4.0f;
        if ((turnCosine < MaxTurnCosine) || ((step > MinJump) && (step > (prevEyeStep * MinJumpRatio)))) {
            return true;
        }

        const float MaxStillStep = 0.05f;
        const float MaxStillTurnCosine = 0.9397f;
        const float MinStillJump = 4.0f;
        const bool wasStill = (prevEyeStep >= 0.0f) && (prevEyeStep < MaxStillStep);
        if (wasStill && ((turnCosine < MaxStillTurnCosine) || (step > MinStillJump))) {
            return true;
        }

        return false;
    }

    struct CameraMove {
        float nearDistance = 0.0f;
        float turnCosine = 1.0f;
        float step = -1.0f;
        hlslpp::float3x3 rotation;
    };

    // How a perspective camera moved from one game frame to the next: its turn, and its eye's step in near plane
    // distances. False for a camera these can't be read from (no near plane, a matrix that isn't finite or can't be
    // inverted), which is never called a cut.
    //
    // A game can put only the camera's turn in the projection, and its place in each world matrix, so that the world
    // moves past a camera that never moves. Excitebike 64 does on its outdoor tracks (each section of the track is
    // drawn at its place less the camera's). A perspective turned by a view with no translation keeps a perspective's
    // last row, so RT64 leaves it whole as the projection, with no view (isMatrixViewProj()), and the turn read 0: the
    // turn is split from it here. The step is -1 when the view leaves the eye at the world's origin in both frames, and
    // worldEyeStep() reads it from the world. Excitebike's credits fly-through cuts from one camera to another on the
    // same track, by turning it 19-173 degrees, and each section and tree, paired with itself or a tree of the same
    // model in the other shot, was drawn halfway between the two places: a trunk filled the picture for a frame. The
    // cut of 19 degrees moves the world 49 near plane distances from a camera that held still.
    static bool measureCameraMove(const hlslpp::float4x4 &curViewProj, const hlslpp::float4x4 &prevViewProj, const hlslpp::float4x4 &curSplitView, const hlslpp::float4x4 &curSplitProj, const hlslpp::float4x4 &prevSplitView, CameraMove &move) {
        hlslpp::float4x4 curView = curSplitView;
        hlslpp::float4x4 curProj = curSplitProj;
        hlslpp::float4x4 prevView = prevSplitView;
        if (isMatrixIdentity(curView) && isMatrixIdentity(prevView)) {
            hlslpp::float4x4 prevProj;
            matrixDecomposeViewProj(curViewProj, curView, curProj);
            matrixDecomposeViewProj(prevViewProj, prevView, prevProj);
        }

        // The near plane's distance from a GL-style perspective (w = z * m23), whatever the scale it was built at.
        const float nearDenominator = float(curProj[2][2]) + float(curProj[2][3]);
        const float nearDistance = (nearDenominator != 0.0f) ? fabsf(float(curProj[3][2]) / nearDenominator) : 0.0f;
        if (!std::isfinite(nearDistance) || (nearDistance <= 0.0f)) {
            return false;
        }

        const hlslpp::float3x3 curRotation = extract3x3(curView);
        const hlslpp::float3x3 prevRotation = extract3x3(prevView);
        const float curDeterminant = hlslpp::determinant(curRotation);
        const float prevDeterminant = hlslpp::determinant(prevRotation);
        if (!std::isfinite(curDeterminant) || !std::isfinite(prevDeterminant) || (curDeterminant == 0.0f) || (prevDeterminant == 0.0f)) {
            return false;
        }

        // The inverse's rows are the camera's axes in the world, and its eye is where the view moves the origin from.
        const hlslpp::float3x3 curInverse = hlslpp::inverse(curRotation);
        const hlslpp::float3x3 prevInverse = hlslpp::inverse(prevRotation);
        const hlslpp::float3 curEye = -hlslpp::mul(hlslpp::float3(curView[3].xyz), curInverse);
        const hlslpp::float3 prevEye = -hlslpp::mul(hlslpp::float3(prevView[3].xyz), prevInverse);
        auto atOrigin = [](const hlslpp::float4x4 &view) {
            return (float(view[3][0]) == 0.0f) && (float(view[3][1]) == 0.0f) && (float(view[3][2]) == 0.0f);
        };

        const float step = (atOrigin(curView) && atOrigin(prevView)) ? -1.0f : float(hlslpp::length(curEye - prevEye)) / nearDistance;
        // The cosine of the rotation's angle, from its trace: the sum of each axis's cosine to its old place is 1 + 2 cos.
        float axisCosines = 0.0f;
        for (int a = 0; a < 3; a++) {
            axisCosines += float(hlslpp::dot(hlslpp::normalize(curInverse[a]), hlslpp::normalize(prevInverse[a])));
        }

        const float turnCosine = (axisCosines - 1.0f) / 2.0f;
        if (!std::isfinite(step) || !std::isfinite(turnCosine)) {
            return false;
        }

        move.nearDistance = nearDistance;
        move.turnCosine = turnCosine;
        move.step = step;
        move.rotation = curRotation;
        return true;
    }

    // The step of a camera whose eye the view leaves at the world's origin (measureCameraMove()), in near plane
    // distances along the view's axes: what most of the scene's paired transforms moved by from the frame before,
    // exactly alike, since what holds still in the world moves by the camera's step alone. -1 when fewer than 4 moved
    // alike: a game whose world matrices hold the camera's turn as well moves each by another step as it turns.
    static float worldEyeStep(const std::vector<MatchCandidate> &candidates, const DrawData &curDrawData, const DrawData &prevDrawData, const hlslpp::float3x3 &viewRotation, float nearDistance) {
        struct TransformMove {
            float x;
            float y;
            float z;
            uint32_t curIndex;

            bool operator<(const TransformMove &o) const {
                if (x != o.x) {
                    return x < o.x;
                }

                if (y != o.y) {
                    return y < o.y;
                }

                return (z != o.z) ? (z < o.z) : (curIndex < o.curIndex);
            }

            bool sameMove(const TransformMove &o) const {
                return (x == o.x) && (y == o.y) && (z == o.z);
            }
        };

        thread_local std::vector<TransformMove> moves;
        moves.clear();
        for (const MatchCandidate &candidate : candidates) {
            const interop::float4x4 &curTransform = curDrawData.worldTransforms[candidate.curIndex];
            const interop::float4x4 &prevTransform = prevDrawData.worldTransforms[candidate.prevIndex];
            const TransformMove move = { curTransform.m[3][0] - prevTransform.m[3][0], curTransform.m[3][1] - prevTransform.m[3][1], curTransform.m[3][2] - prevTransform.m[3][2], candidate.curIndex };
            if (std::isfinite(move.x) && std::isfinite(move.y) && std::isfinite(move.z)) {
                moves.emplace_back(move);
            }
        }

        // The move the most transforms made, each counted once.
        std::sort(moves.begin(), moves.end());
        const uint32_t MinAlike = 4;
        uint32_t bestCount = 0;
        size_t bestIndex = 0;
        for (size_t i = 0; i < moves.size();) {
            size_t j = i;
            uint32_t count = 0;
            while ((j < moves.size()) && moves[j].sameMove(moves[i])) {
                if ((j == i) || (moves[j].curIndex != moves[j - 1].curIndex)) {
                    count++;
                }

                j++;
            }

            if (count > bestCount) {
                bestCount = count;
                bestIndex = i;
            }

            i = j;
        }

        if (bestCount < MinAlike) {
            return -1.0f;
        }

        const TransformMove &best = moves[bestIndex];
        const float step = float(hlslpp::length(hlslpp::mul(hlslpp::float3(best.x, best.y, best.z), viewRotation))) / nearDistance;
        return std::isfinite(step) ? step : -1.0f;
    }
#endif

#if RT64_SKIP_PLACE_SWAPS
    // A transform's place, its world matrix's translation, to find whether a frame drew anything at exactly that place:
    // what stands still in a game's world is drawn at the very same translation each frame.
    struct TransformPlace {
        float x;
        float y;
        float z;

        bool operator<(const TransformPlace &o) const {
            if (x != o.x) {
                return x < o.x;
            }

            return (y != o.y) ? (y < o.y) : (z < o.z);
        }

        bool operator==(const TransformPlace &o) const {
            return (x == o.x) && (y == o.y) && (z == o.z);
        }

        bool isFinite() const {
            return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        }
    };

    static TransformPlace transformPlace(const interop::float4x4 &transform) {
        return { transform.m[3][0], transform.m[3][1], transform.m[3][2] };
    }

    // Every finite place a workload drew a transform at, sorted, without repeats.
    static void collectTransformPlaces(const Workload &workload, std::vector<TransformPlace> &places) {
        places.clear();
        for (const interop::float4x4 &transform : workload.drawData.worldTransforms) {
            const TransformPlace place = transformPlace(transform);
            if (place.isFinite()) {
                places.emplace_back(place);
            }
        }

        std::sort(places.begin(), places.end());
        places.erase(std::unique(places.begin(), places.end()), places.end());
    }

    // A hash of each transform's vertices, as many and where they are: one mesh from another.
    static void collectTransformMeshes(const Workload &workload, std::vector<uint64_t> &meshes) {
        const DrawData &drawData = workload.drawData;
        const uint32_t transformCount = uint32_t(drawData.worldTransformVertexIndices.size());
        meshes.resize(transformCount);
        for (uint32_t i = 0; i < transformCount; i++) {
            const uint32_t vertexIndex = drawData.worldTransformVertexIndices[i];
            const uint32_t vertexCount = drawData.worldTransformVertexCount(i);
            meshes[i] = (vertexCount > 0) ? XXH3_64bits_withSeed(&drawData.posFloats[vertexIndex * 3], vertexCount * 3 * sizeof(float), vertexCount) : 0;
        }
    }
#endif

    TransformMatchResult computeTransformMatch(const hlslpp::float4x4 &curTransform, const hlslpp::float4x4 &curViewProj, const hlslpp::float4x4 &prevTransform, const hlslpp::float4x4 &prevViewProj, const RigidBody *prevRigidBody) {
        TransformMatchResult matchResult;

        // Do not accept a match between these transforms if the determinant is different, which indicates they're mirrored from each other.
        const float m0det = hlslpp::determinant(extract3x3(prevTransform));
        const float m1det = hlslpp::determinant(extract3x3(curTransform));
        if ((m0det * m1det) < 0.0f) {
            return matchResult;
        }

#   if RT64_SKIP_COLLAPSED_MATCHES
        // Nor between one that collapses what it draws (a zero determinant) and one that doesn't: a game hides an object
        // by scaling it to 0 or loading a matrix of zeros, and the frames between drew a matrix that no game frame had.
        // Mario Party 2's board hides the star it hands out with matrices of zeros, which were paired with matrices of
        // the frame before that still drew it, and the frames between drew its triangles across the whole view in
        // front of the board. Unpaired, each is drawn at its own matrix between the frames: what the game hid draws
        // nothing there, as in the game's frame, and what comes back from 0 is at its place from the frame it comes.
        if ((m0det == 0.0f) != (m1det == 0.0f)) {
            return matchResult;
        }
#   endif

        // Compute the difference between the translation components of the 4x4 matrices.
        const hlslpp::float3 curPos = curTransform[3].xyz;
        hlslpp::float3 prevPos = prevTransform[3].xyz;
        if (prevRigidBody != nullptr) {
            prevPos += prevRigidBody->linearVelocity;
        }

        matchResult.positionDifference = hlslpp::length(curPos - prevPos);

        // Compute the dot product difference between the normalized XYZ vectors of the 3x3 matrices.
        matchResult.orientationDifference =
            (1.0f - hlslpp::dot(hlslpp::normalize(curTransform[0].xyz), hlslpp::normalize(prevTransform[0].xyz))) +
            (1.0f - hlslpp::dot(hlslpp::normalize(curTransform[1].xyz), hlslpp::normalize(prevTransform[1].xyz))) +
            (1.0f - hlslpp::dot(hlslpp::normalize(curTransform[2].xyz), hlslpp::normalize(prevTransform[2].xyz)));

        // And how far apart their scales are, as the ratio of each axis's length, since the orientation's axes are
        // normalized. Parts of one object that a game draws at one place and orientation and only scales apart tied
        // without it, and were paired across, one part's frames between drawn toward another's size and place.
        matchResult.scaleDifference = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float curLength = hlslpp::length(curTransform[i].xyz);
            const float prevLength = hlslpp::length(prevTransform[i].xyz);
            if ((curLength > 0.0f) && (prevLength > 0.0f)) {
                matchResult.scaleDifference += fabsf(logf(curLength / prevLength));
            }
        }

        // Compute the difference between the screen-space position of both transforms on their respective projections.
        hlslpp::float4 prevScreenPos = hlslpp::mul(prevTransform[3], prevViewProj);
        hlslpp::float4 curScreenPos = hlslpp::mul(curTransform[3], curViewProj);
        prevScreenPos = (fabs(prevScreenPos.w) < 1e-6f) ? prevScreenPos : prevScreenPos / prevScreenPos.w;
        curScreenPos = (fabs(curScreenPos.w) < 1e-6f) ? curScreenPos : curScreenPos / curScreenPos.w;
        matchResult.screenSpaceDifference = hlslpp::length(curScreenPos.xyz - prevScreenPos.xyz);
        
        matchResult.valid = true;
        return matchResult;
    }

#if RT64_GAME_FRAME_WORKLOADS
    void GameFrame::alignDrawnWorkloads(const WorkloadQueue &workloadQueue, const std::vector<uint32_t> &curDrawn, const std::vector<uint32_t> &prevDrawn, std::vector<std::pair<uint32_t, uint32_t>> &drawnPairs) {
        // Two frames with as many drawn workloads pair them in order. When one draws more (a close-up in a corner that
        // comes or goes), pairing in order would match every workload after the one that is missing with another's,
        // and what it draws would be interpolated from another camera's place for a frame. So the longer list's extra
        // workloads are left out where the cameras of the pairs that are left differ the least, still in order.
        //
        // A workload's camera is its first perspective projection's, or its first orthographic one's if it has none.
        auto workloadCamera = [&](uint32_t workloadIndex) -> const Projection * {
            const Workload &workload = workloadQueue.workloads[workloadIndex];
            const Projection *orthographic = nullptr;
            for (uint32_t f = 0; f < workload.fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                for (uint32_t p = 0; p < fbPair.projectionCount; p++) {
                    const Projection &proj = fbPair.projections[p];
                    if (proj.type == Projection::Type::Perspective) {
                        return &proj;
                    }
                    else if ((proj.type == Projection::Type::Orthographic) && (orthographic == nullptr)) {
                        orthographic = &proj;
                    }
                }
            }

            return orthographic;
        };

        auto cameraDifference = [&](uint32_t curIndex, uint32_t prevIndex) -> double {
            const Projection *curProj = workloadCamera(curIndex);
            const Projection *prevProj = workloadCamera(prevIndex);
            if ((curProj == nullptr) || (prevProj == nullptr)) {
                return (curProj == prevProj) ? 0.0 : 1e12;
            }
            else if (curProj->type != prevProj->type) {
                return 1e12;
            }

            // A camera that isn't finite (one a game draws a shadow into a texture with can have a projection of NaNs)
            // would make every cost after it NaN: it differs as much as cameras of two types do.
            const Workload &curWorkload = workloadQueue.workloads[curIndex];
            const Workload &prevWorkload = workloadQueue.workloads[prevIndex];
            const double difference = double(matrixDifference(curWorkload.drawData.viewTransforms[curProj->transformsIndex], prevWorkload.drawData.viewTransforms[prevProj->transformsIndex])) +
                double(matrixDifference(curWorkload.drawData.projTransforms[curProj->transformsIndex], prevWorkload.drawData.projTransforms[prevProj->transformsIndex]));
            return std::isfinite(difference) ? difference : 1e12;
        };

        // Every workload of the shorter list is paired with one of the longer's, in order, for the least difference in
        // all: cost(i, j) is the least for the longer's first i and the shorter's first j.
        const bool curLonger = curDrawn.size() > prevDrawn.size();
        const std::vector<uint32_t> &longer = curLonger ? curDrawn : prevDrawn;
        const std::vector<uint32_t> &shorter = curLonger ? prevDrawn : curDrawn;
        const size_t n = longer.size();
        const size_t m = shorter.size();
        auto pairDifference = [&](size_t i, size_t j) {
            return curLonger ? cameraDifference(longer[i], shorter[j]) : cameraDifference(shorter[j], longer[i]);
        };

        thread_local std::vector<double> costs;
        costs.assign((n + 1) * (m + 1), 0.0);
        auto cost = [&](size_t i, size_t j) -> double & { return costs[i * (m + 1) + j]; };
        for (size_t i = 1; i <= n; i++) {
            for (size_t j = 1; (j <= m) && (j <= i); j++) {
                const double paired = cost(i - 1, j - 1) + pairDifference(i - 1, j - 1);
                cost(i, j) = (j < i) ? std::min(paired, cost(i - 1, j)) : paired;
            }
        }

        // Walked back from the end, a tie leaves out the later workload, so lists whose cameras all agree keep the
        // pairing in order.
        size_t i = n;
        size_t j = m;
        while (j > 0) {
            if ((j < i) && (cost(i - 1, j) <= cost(i, j))) {
                i--;
                continue;
            }

            drawnPairs.emplace_back(curLonger ? longer[i - 1] : shorter[j - 1], curLonger ? shorter[j - 1] : longer[i - 1]);
            i--;
            j--;
        }

        std::reverse(drawnPairs.begin(), drawnPairs.end());
    }
#endif

#if RT64_INTERPOLATE_RECT_SCROLLS
    // A rectangle (a texrect, drawn without a viewport) is never paired with one of the game frame before, so a
    // texture the game scrolls inside a rect that stays put (a horizon panorama that turns with the camera, a sky or a
    // menu's backdrop) steps once a game frame, and so does a rect the game moves (the same panorama as the camera
    // pitches), while the world interpolated around it moves on every frame. Each rect here is paired with the next of
    // the frame before's in its order that drew the same place with the same textures (its rect, scale and tiles, and
    // each tile's TMEM hash), or, with none, with the nearest that drew the same textures at the same size and scale
    // less than its longer side away each way: a strip as wide as the screen and a few rows high follows a camera that
    // pitches fast, and a glyph of a font doesn't pair with the same glyph a cell or more away. One that far, farther
    // than its shorter side, is passed over when one of another size with the same textures lies nearer: the same
    // textures drawn in several places, each cut by the screen's edge as they slide off it, a frame or two apart (Mario
    // Golf's save menu, three slots of one wooden strip), take on each other's sizes, and a slot paired with the one
    // above it, its own cut strip being nearer. That is the rect itself, cut to another size, which the last pass
    // pairs. A rect drawn in the same place in both frames' order of rects, the frames having drawn the same ones in
    // the same order before and after it, pairs with that one alone, in every pass, or with none (but by place first
    // when another rect now stands in that one's place with its look, the same things drawn in another order): a
    // glyph of a line of text the game slides faster than a glyph's size a game frame paired with the same letter of
    // another word, or its shadow's, so the frames between drew those letters a few pixels from the rest. When every corner's
    // texcoords moved by the same amount, less than a quarter of the texels the rect spans in each direction (a step
    // across a wrapped tile's period taken inside it), its tiles are placed that much back in the frame before, so the
    // tile processor slides the texture there in the frames between, and a rect that moved is drawn that much nearer
    // where it stood (rectMoves, which the framebuffer renderer reads). A rect whose texture jumped (another glyph of a
    // font, another frame of an animation) shows its new texels unslid, and is still drawn nearer where it stood when it
    // moved. Last, a rect still unpaired pairs with the nearest that drew the same textures at the same scale with a
    // size of its own, where each way its edges stayed, moved together less than its side, one stayed and the other
    // moved less than a quarter of its side, both moved the same way less than its side and a pixel apart at most, or
    // one stayed and the other moved less than its longer side with its texels: a panorama's last strip that the game
    // draws on to the bottom of the screen, its top following the strips above as the camera pitches and its bottom
    // held there, a strip of a sprite that moves by fractions of a pixel with its edges put on whole pixels, a pixel
    // taller in some frames than in others, or a panorama's strip that crosses the top of the picture, a few rows high,
    // its top held at the scissor's and its texcoord there stepping a row as its bottom moves a pixel (Mario Golf's
    // hills as the camera pans: left unpaired, a row of the sky showed under it and its hills stood aside of those
    // below in the frames between). That strip's texture slides by its step, however much of the rect's few rows it
    // is. Each of a rect's
    // edges is drawn between where it was and where it is, its texcoords at its upper left, so it meets the strip above
    // in every frame between and leaves no row of what is behind it showing. One still unpaired pairs with the nearest,
    // less than its longer side away, that drew the same textures at another scale with the same texels at its corners,
    // within a texel: a sprite the game grows or shrinks as it moves it (Mario Golf's Play Modes wheel, each mode's
    // icon drawn in two strips at a scale of its own as the wheel turns), each of whose edges is drawn between where it
    // was and where it is, its texture stretched across it as the game stretches it. Left unpaired, its strips slid in
    // the game frames whose scale stayed and stepped in the rest, off the pace of the wheel. A rect that changed
    // texture, or its size any other way, is drawn where the game put it. Places and sizes are compared as the game
    // drew them, before the enhancement that moves an edge within a pixel of the scissor's onto it (rect.fixRectLR): it
    // grows or shrinks a rect by a pixel in the frames its edge passes the scissor's, and a glyph that changed size
    // there paired with its shadow, the same glyph drawn a pixel aside, so the frames between drew the line a pixel to
    // the side. Last, a strip still unpaired that the frame before drew nothing of slides with the strip of its
    // panorama it adjoins (below). A rect's edge that came onto the scissor's from inside it, where the game cuts it as
    // the panorama moves on, is held there in the frames between, its texels moving with its other edge, as at an edge
    // that stayed: the strip of Mario Golf's hills under one that has just left the picture over the top as the camera
    // pitches down, drawn between where its top was and the scissor, showed the clear color's dark blue in the
    // picture's top row. A rect drawn from a tile copy (a picture of the framebuffer) pairs by what the copy holds,
    // the framebuffer and the part of it copied (DrawCallTile::tileCopySource), not by the copy's ID, which stands in
    // for its TMEM hash: a copy is reused for whichever copy of its size is made next, so the same ID in the frame
    // before is another picture or another part of it. Mario Golf's intro fades one shot into the next by copying the
    // old shot into a buffer in 38 strips the first frame and drawing them back over the new shot every frame, each
    // strip a copy of its part of the buffer: in the second frame each strip paired by its ID with a copy strip of the
    // first that many rows lower, and the frames between drew the top half of the old shot slid down, the new shot
    // uncovered above it.
    // Rects that sample no texture are paired among themselves, apart from the textured ones, so a rect of either kind
    // never changes what the other's runs of owns are: one that draws a colour (a combiner of primitive or environment
    // colour alone, or a fill) has the same look as one that draws it with the same combiner, other modes and colours,
    // and pairs, as a textured one does, in its place, or with the nearest of its size less than its longer side or 16
    // pixels away. It is never paired with one of another size, having no texels to say it is the same thing cut. One
    // that moved is drawn between where it was and where it is, and at the corners the game gave it to a quarter pixel
    // (the framebuffer renderer): Mario Golf draws a ball far from the camera as a dot of primitive colour two pixels
    // by one, which held its place for a game frame and then stepped a pixel. One the frame before drew a rect of
    // colour of its size in the same place, in another colour, changed colour where it stands, and pairs with no
    // look-alike elsewhere: a fade drawn in columns whose alpha ramps across the screen as it sweeps (Excitebike 64's,
    // 64 columns as high as the screen) paired each column with the one that had its alpha a game frame before, up to
    // the screen's height away, and the frames between slid the columns from there, a strip of the picture uncovered
    // where the ramp met the black.
    // A textured rect whose texture jumped as it changed size or moved half of itself along its row, where the frame
    // before drew other rects with its look (the glyphs of a font packed into one texture page), is another picture there,
    // drawn where the game put it, unless the jump is a cut at the scissor's edge (below). So is one that moved along its
    // row onto the place of a rect nothing paired with while another took the place it left, a line of glyphs whose values
    // changed.
    static void matchRectScrolls(const Workload &curWorkload, const Workload &prevWorkload, GameFrameMap::WorkloadMap &workloadMap, bool &tileInterpolationUsed, bool untextured) {
        struct RectCall {
            const DrawCall *call;
            uint32_t rawVertex;
        };

        auto collect = [untextured](const Workload &workload, std::vector<RectCall> &rects) {
            rects.clear();
            uint32_t rawVertex = 0;
            const size_t rawVertexCount = workload.drawData.triTcFloats.size() / 2;
            for (uint32_t f = 0; f < workload.fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                for (uint32_t p = 0; p < fbPair.projectionCount; p++) {
                    const Projection &proj = fbPair.projections[p];
                    if (proj.usesViewport()) {
                        continue;
                    }

                    for (uint32_t c = 0; c < proj.gameCallCount; c++) {
                        const DrawCall &call = proj.gameCalls[c].callDesc;
                        const bool oneRect = (call.triangleCount == 2) && ((rawVertex + 6) <= rawVertexCount);
                        const bool kind = untextured ? (call.tileCount == 0) : (call.textureOn && (call.tileCount > 0) && (call.tileCount <= 8));
                        if ((proj.type == Projection::Type::Rectangle) && oneRect && kind) {
                            rects.push_back({ &call, rawVertex });
                        }

                        rawVertex += call.triangleCount * 3;
                    }
                }
            }
        };

        thread_local std::vector<RectCall> curRects;
        thread_local std::vector<RectCall> prevRects;
        thread_local std::vector<bool> prevUsed;
        collect(curWorkload, curRects);
        collect(prevWorkload, prevRects);
        prevUsed.assign(prevRects.size(), false);

        const DrawData &curData = curWorkload.drawData;
        const DrawData &prevData = prevWorkload.drawData;
        auto sameSize = [](const DrawCall &a, const DrawCall &b) {
            return ((a.gameRect.lrx - a.gameRect.ulx) == (b.gameRect.lrx - b.gameRect.ulx)) && ((a.gameRect.lry - a.gameRect.uly) == (b.gameRect.lry - b.gameRect.uly));
        };

        // The same colours drawn the same way, for a rect that samples no texture.
        auto sameColor = [](const hlslpp::float4 &a, const hlslpp::float4 &b) {
            return (float(a.x) == float(b.x)) && (float(a.y) == float(b.y)) && (float(a.z) == float(b.z)) && (float(a.w) == float(b.w));
        };

        // The same textures, whatever the rect's size and scale; a tile copy's by what it holds, not by its ID.
        auto sameTiles = [&](const DrawCall &a, const DrawCall &b) {
            if (a.tileCount != b.tileCount) {
                return false;
            }

            if (untextured) {
                return (a.colorCombiner.L == b.colorCombiner.L) && (a.colorCombiner.H == b.colorCombiner.H) && (a.otherMode.L == b.otherMode.L) &&
                    (a.otherMode.H == b.otherMode.H) && (a.fillColor == b.fillColor) && sameColor(a.rdpParams.primColor, b.rdpParams.primColor) &&
                    sameColor(a.rdpParams.envColor, b.rdpParams.envColor);
            }

            for (uint32_t t = 0; t < a.tileCount; t++) {
                const DrawCallTile &ca = curData.callTiles[a.tileIndex + t];
                const DrawCallTile &cb = prevData.callTiles[b.tileIndex + t];
                const bool sameTexels = (ca.tileCopyUsed || cb.tileCopyUsed) ?
                    (ca.tileCopyUsed && cb.tileCopyUsed && (ca.tileCopySource == cb.tileCopySource)) :
                    (ca.tmemHashOrID == cb.tmemHashOrID);
                const interop::RDPTile &ta = curData.rdpTiles[a.tileIndex + t];
                const interop::RDPTile &tb = prevData.rdpTiles[b.tileIndex + t];
                if (!sameTexels ||
                    (ta.fmt != tb.fmt) || (ta.siz != tb.siz) || (ta.palette != tb.palette) || (ta.masks != tb.masks) || (ta.maskt != tb.maskt) ||
                    (ta.cms != tb.cms) || (ta.cmt != tb.cmt) || (ta.shifts != tb.shifts) || (ta.shiftt != tb.shiftt) ||
                    (ta.uls != tb.uls) || (ta.ult != tb.ult) || (ta.lrs != tb.lrs) || (ta.lrt != tb.lrt))
                {
                    return false;
                }
            }

            return true;
        };

        // The same textures at the same scale, whatever the rect's size.
        auto sameLook = [&](const DrawCall &a, const DrawCall &b) {
            return (a.rectDsdx == b.rectDsdx) && (a.rectDtdy == b.rectDtdy) && sameTiles(a, b);
        };

        auto sameTextures = [&](const DrawCall &a, const DrawCall &b) {
            return sameSize(a, b) && sameLook(a, b);
        };

        auto sameRect = [&](const DrawCall &a, const DrawCall &b) {
            return (a.gameRect.ulx == b.gameRect.ulx) && (a.gameRect.uly == b.gameRect.uly) && sameTextures(a, b);
        };

        // Moved by less than the rect's longer side each way, in quarter pixels, or, for one with no texture, less than
        // 16 pixels: a dot of colour has no glyph of a font to be taken for, and Mario Golf's far ball, two pixels by
        // one, falls 3 to 6 pixels a game frame at the end of a drive.
        auto movedLess = [untextured](const DrawCall &a, const DrawCall &b) {
            const int32_t side = std::max(untextured ? (16 * 4) : 0, std::max(a.gameRect.lrx - a.gameRect.ulx, a.gameRect.lry - a.gameRect.uly));
            return (abs(b.gameRect.ulx - a.gameRect.ulx) < side) && (abs(b.gameRect.uly - a.gameRect.uly) < side);
        };

        // Farther than that, less than four times its longer side each way, a rect pairs with one of the frame before
        // that drew the same textures at the same size and scale when it is the only rect of each frame with that look:
        // with no look-alike to be taken for, it is the same thing however fast the game moves it, and four of its sides
        // still leave out a jump to another place on the screen. Mario Golf's pin flag in the sky, 8 by 16 pixels,
        // which a fast turn of the view moves 22 to 26 pixels a game frame, was left unpaired, held still in the frames
        // between and stepped once a game frame against the course turning behind it.
        auto onlyOneFar = [&](const RectCall &ra, size_t j) {
            const DrawCall &a = *ra.call;
            const DrawCall &b = *prevRects[j].call;
            const int32_t side = std::max(a.gameRect.lrx - a.gameRect.ulx, a.gameRect.lry - a.gameRect.uly) * 4;
            if (untextured || (abs(b.gameRect.ulx - a.gameRect.ulx) >= side) || (abs(b.gameRect.uly - a.gameRect.uly) >= side) || !sameTextures(a, b)) {
                return false;
            }
            for (size_t k = 0; k < prevRects.size(); k++) {
                if ((k != j) && sameLook(a, *prevRects[k].call)) {
                    return false;
                }
            }
            for (const RectCall &c : curRects) {
                if ((&c != &ra) && sameLook(*c.call, b)) {
                    return false;
                }
            }
            return true;
        };

        // The edge the game holds one way, of a rect (a) and the one it pairs with in the frame before (b): 1 its upper
        // left, 2 its lower right, 0 neither. One that stayed while the other moved, or, with both moved, one that came
        // onto the scissor's from inside it, where the game cuts the rect: the strip of a panorama under one that left
        // the picture over the top, whole in the frame before and cut at the top in this one (Mario Golf's hills as
        // the camera pitches down).
        auto heldEdge = [](const DrawCall &ca, const DrawCall &cb, bool vertical) {
            const int32_t aUl = vertical ? ca.gameRect.uly : ca.gameRect.ulx;
            const int32_t aLr = vertical ? ca.gameRect.lry : ca.gameRect.lrx;
            const int32_t bUl = vertical ? cb.gameRect.uly : cb.gameRect.ulx;
            const int32_t bLr = vertical ? cb.gameRect.lry : cb.gameRect.lrx;
            const int32_t ulMove = bUl - aUl;
            const int32_t lrMove = bLr - aLr;
            if ((ulMove == 0) != (lrMove == 0)) {
                return (ulMove == 0) ? 1 : 2;
            }

            if (ulMove == 0) {
                return 0;
            }

            const bool ulOnto = (aUl <= (vertical ? ca.scissorRect.uly : ca.scissorRect.ulx)) && (ulMove > 0);
            const bool lrOnto = (aLr >= (vertical ? ca.scissorRect.lry : ca.scissorRect.lrx)) && (lrMove < 0);
            return (ulOnto == lrOnto) ? 0 : (ulOnto ? 1 : 2);
        };

        // Whether the edge the game holds one way (heldEdge) lies on the scissor's, within a pixel: where it cuts a rect
        // at the screen's edge, as a panorama or a sliding menu goes on past it.
        auto heldAtScissor = [&](const DrawCall &ca, const DrawCall &cb, bool vertical) {
            const int held = heldEdge(ca, cb, vertical);
            if (held == 1) {
                return (vertical ? ca.gameRect.uly : ca.gameRect.ulx) <= ((vertical ? ca.scissorRect.uly : ca.scissorRect.ulx) + 4);
            }

            if (held == 2) {
                return (vertical ? ca.gameRect.lry : ca.gameRect.lrx) >= ((vertical ? ca.scissorRect.lry : ca.scissorRect.lrx) - 4);
            }

            return false;
        };

        // A rect the game cuts at an edge it holds (a strip of a panorama that crosses the top of the picture, held at
        // the scissor as the panorama moves on under it): that edge held (heldEdge) and the other moved less than the
        // rect's longer side, and its texels moved with that edge, the texcoord at the held edge stepping by what it
        // moved (where the texture's texel 0 falls, in pixels, moved by as much as the edge, within a quarter pixel).
        // Its texture slides by that step, however large a part of the few texels the rect spans it is.
        auto cutAt = [&](const RectCall &a, const RectCall &b, bool vertical) {
            const DrawCall &ca = *a.call;
            const DrawCall &cb = *b.call;
            const int32_t aUl = vertical ? ca.gameRect.uly : ca.gameRect.ulx;
            const int32_t aLr = vertical ? ca.gameRect.lry : ca.gameRect.lrx;
            const int32_t bUl = vertical ? cb.gameRect.uly : cb.gameRect.ulx;
            const int32_t bLr = vertical ? cb.gameRect.lry : cb.gameRect.lrx;
            const int held = heldEdge(ca, cb, vertical);
            const int32_t edgeMove = (held == 1) ? (bLr - aLr) : (bUl - aUl);
            if ((held == 0) || (abs(edgeMove) >= std::max(aLr - aUl, bLr - bUl))) {
                return false;
            }

            const float aScale = float(vertical ? ca.rectDtdy : ca.rectDsdx) / 1024.0f;
            const float bScale = float(vertical ? cb.rectDtdy : cb.rectDsdx) / 1024.0f;
            if ((aScale == 0.0f) || (aScale != bScale)) {
                return false;
            }

            // Texcoords follow the rect drawn, so texel 0's place is the drawn upper left's less its texcoord.
            const uint32_t axis = vertical ? 1 : 0;
            const float aAt = (float(vertical ? ca.rect.uly : ca.rect.ulx) / 4.0f) - (curData.triTcFloats[a.rawVertex * 2 + axis] / aScale);
            const float bAt = (float(vertical ? cb.rect.uly : cb.rect.ulx) / 4.0f) - (prevData.triTcFloats[b.rawVertex * 2 + axis] / bScale);
            return fabsf((bAt - aAt) - (float(edgeMove) / 4.0f)) <= 0.25f;
        };

        // Each way, in quarter pixels, its edges stayed, moved together less than its side, one stayed and the other
        // moved less than a quarter of its side, or both moved the same way less than its side and a pixel apart at
        // most: a strip of a sprite drawn in strips, each edge put on a whole pixel as the sprite moves by fractions of
        // one (a 2D screen the game or its recomp scales), grows or shrinks by a pixel in the frames an edge rounds the
        // other way, and left unpaired it is drawn where the game put it while the strips beside it are drawn between,
        // a row of what is behind them showing. Or, one way, the rect was cut there (cutAt).
        auto edgesFollow = [&](const RectCall &ra, const RectCall &rb) {
            auto follows = [](int32_t aUl, int32_t aLr, int32_t bUl, int32_t bLr) {
                const int32_t side = std::min(aLr - aUl, bLr - bUl);
                const int32_t ulMove = bUl - aUl;
                const int32_t lrMove = bLr - aLr;
                if (ulMove == lrMove) {
                    return abs(ulMove) < side;
                }

                if (((ulMove == 0) && ((abs(lrMove) * 4) < side)) || ((lrMove == 0) && ((abs(ulMove) * 4) < side))) {
                    return true;
                }

                const bool sameWay = (ulMove != 0) && (lrMove != 0) && ((ulMove > 0) == (lrMove > 0));
                return sameWay && (abs(ulMove) < side) && (abs(lrMove) < side) && (abs(lrMove - ulMove) <= 4);
            };

            const DrawCall &a = *ra.call;
            const DrawCall &b = *rb.call;
            return (follows(a.gameRect.ulx, a.gameRect.lrx, b.gameRect.ulx, b.gameRect.lrx) || cutAt(ra, rb, false)) &&
                (follows(a.gameRect.uly, a.gameRect.lry, b.gameRect.uly, b.gameRect.lry) || cutAt(ra, rb, true));
        };

        // The same texels at its corners, within a texel each way: the least and most texcoord over its vertices.
        auto sameCorners = [&](const RectCall &a, const RectCall &b) {
            const float *aTc = &curData.triTcFloats[a.rawVertex * 2];
            const float *bTc = &prevData.triTcFloats[b.rawVertex * 2];
            for (uint32_t axis = 0; axis < 2; axis++) {
                float aMin = aTc[axis], aMax = aTc[axis], bMin = bTc[axis], bMax = bTc[axis];
                for (uint32_t v = 1; v < 6; v++) {
                    aMin = std::min(aMin, aTc[v * 2 + axis]);
                    aMax = std::max(aMax, aTc[v * 2 + axis]);
                    bMin = std::min(bMin, bTc[v * 2 + axis]);
                    bMax = std::max(bMax, bTc[v * 2 + axis]);
                }

                if ((fabsf(aMin - bMin) > 1.0f) || (fabsf(aMax - bMax) > 1.0f)) {
                    return false;
                }
            }

            return true;
        };

        // A step in a tile's texels, taken inside its period when it wraps or mirrors.
        auto insidePeriod = [](float step, int mask, int cm) {
            const float period = float(mask) * ((cm & G_TX_MIRROR) ? 2.0f : 1.0f);
            if ((cm & G_TX_CLAMP) || (period <= 0.0f)) {
                return step;
            }

            step = fmodf(step, period);
            if (step > (period / 2.0f)) {
                step -= period;
            }
            else if (step < -(period / 2.0f)) {
                step += period;
            }

            return step;
        };

        // A rect's own in the frame before: where both frames drew the same rects in the same order (the same textures
        // at the same scale, call by call, from the start and from the end), the rect drawn in its place in that
        // order. One that has its own pairs with it alone, moved or cut, never with a look-alike: a line of text is
        // the same few glyphs over and over, a glyph's rect is often wider than the space it takes, and the line may
        // be drawn twice, a pixel apart, for its shadow, so a line the game slides faster than a glyph's size a game
        // frame paired a glyph with the same letter of another word, or with its shadow, and the frames between drew
        // those letters a few pixels from where the game put the rest (Mario Golf's save menu as it slides off:
        // "There is no saved data" drawn "Ther eis nos aved dta"). Unpaired, it is drawn where the game put it, as the
        // rest of its line is. Where the frames drew a different number of rects, a rect that both the run from the
        // start and the run from the end reach has two places, one rect apart or more, with the same look in each (a
        // repeated letter where a glyph came or went): it has no own, and pairs as it did before there were owns.
        const size_t commonCount = std::min(curRects.size(), prevRects.size());
        size_t head = 0;
        while ((head < commonCount) && sameLook(*curRects[head].call, *prevRects[head].call)) {
            head++;
        }

        size_t tail = 0;
        while ((tail < commonCount) && sameLook(*curRects[curRects.size() - 1 - tail].call, *prevRects[prevRects.size() - 1 - tail].call)) {
            tail++;
        }

        thread_local std::vector<size_t> curOwn;
        curOwn.assign(curRects.size(), prevRects.size());
        for (size_t r = 0; r < head; r++) {
            curOwn[r] = r;
        }

        for (size_t t = 0; t < tail; t++) {
            const size_t c = curRects.size() - 1 - t;
            const size_t p = prevRects.size() - 1 - t;
            curOwn[c] = ((c < head) && (c != p)) ? prevRects.size() : p;
        }

        // Every rect that drew the same place pairs first, so one that stayed put never pairs with a look-alike that moved.
        thread_local std::vector<size_t> curFound;
        curFound.assign(curRects.size(), prevRects.size());
        size_t searchStart = 0;
        for (size_t r = 0; r < curRects.size(); r++) {
            const size_t own = curOwn[r];
            if (own < prevRects.size()) {
                const DrawCall &ownCall = *prevRects[own].call;
                if (!prevUsed[own] && sameRect(*curRects[r].call, ownCall)) {
                    curFound[r] = own;
                    prevUsed[own] = true;
                    searchStart = own + 1;
                    continue;
                }

                // Its own moved, unless another rect of this frame stands in its place with its look: then the frames
                // drew the same things in the same places in another order (two sprites swapped in the list), and it
                // pairs by place, as one with no own does.
                bool ownStayed = false;
                for (size_t c = 0; (c < curRects.size()) && !ownStayed; c++) {
                    ownStayed = (c != r) && sameRect(*curRects[c].call, ownCall);
                }

                if (!ownStayed) {
                    continue;
                }
            }

            for (size_t i = 0; i < prevRects.size(); i++) {
                const size_t j = (searchStart + i) % prevRects.size();
                if (!prevUsed[j] && sameRect(*curRects[r].call, *prevRects[j].call)) {
                    curFound[r] = j;
                    prevUsed[j] = true;
                    searchStart = j + 1;
                    break;
                }
            }
        }

        for (size_t r = 0; r < curRects.size(); r++) {
            if (curFound[r] < prevRects.size()) {
                continue;
            }

            const DrawCall &curCall = *curRects[r].call;
            if (untextured) {
                // A rect of colour where the frame before drew one of its size, in another colour, stays (above).
                bool placeDrawn = false;
                for (size_t j = 0; (j < prevRects.size()) && !placeDrawn; j++) {
                    const DrawCall &prevCall = *prevRects[j].call;
                    placeDrawn = (prevCall.gameRect.ulx == curCall.gameRect.ulx) && (prevCall.gameRect.uly == curCall.gameRect.uly) && sameSize(curCall, prevCall);
                }

                if (placeDrawn) {
                    continue;
                }
            }

            const size_t own = curOwn[r];
            if (own < prevRects.size()) {
                const DrawCall &ownCall = *prevRects[own].call;
                if (!prevUsed[own] && ((movedLess(curCall, ownCall) && sameTextures(curCall, ownCall)) || onlyOneFar(curRects[r], own))) {
                    curFound[r] = own;
                    prevUsed[own] = true;
                }

                continue;
            }

            int32_t nearest = INT32_MAX;
            for (size_t j = 0; j < prevRects.size(); j++) {
                const DrawCall &prevCall = *prevRects[j].call;
                if (prevUsed[j] || !movedLess(curCall, prevCall) || !sameTextures(curCall, prevCall)) {
                    continue;
                }

                const int32_t distance = abs(prevCall.gameRect.ulx - curCall.gameRect.ulx) + abs(prevCall.gameRect.uly - curCall.gameRect.uly);
                if (distance < nearest) {
                    nearest = distance;
                    curFound[r] = j;
                }
            }

            // None near: the one look-alike of a rect no other shares, farther (onlyOneFar).
            for (size_t j = 0; (j < prevRects.size()) && (curFound[r] == prevRects.size()); j++) {
                if (!prevUsed[j] && onlyOneFar(curRects[r], j)) {
                    curFound[r] = j;
                }
            }

            // Farther than its shorter side, a look-alike of another size nearer is the rect itself, cut to that size.
            if (curFound[r] < prevRects.size()) {
                const DrawCall &foundCall = *prevRects[curFound[r]].call;
                const int32_t shorter = std::min(curCall.gameRect.lrx - curCall.gameRect.ulx, curCall.gameRect.lry - curCall.gameRect.uly);
                if ((abs(foundCall.gameRect.ulx - curCall.gameRect.ulx) >= shorter) || (abs(foundCall.gameRect.uly - curCall.gameRect.uly) >= shorter)) {
                    for (size_t j = 0; j < prevRects.size(); j++) {
                        const DrawCall &prevCall = *prevRects[j].call;
                        const int32_t distance = abs(prevCall.gameRect.ulx - curCall.gameRect.ulx) + abs(prevCall.gameRect.uly - curCall.gameRect.uly);
                        if ((distance < nearest) && !sameSize(curCall, prevCall) && sameLook(curCall, prevCall)) {
                            curFound[r] = prevRects.size();
                            break;
                        }
                    }
                }
            }

            if (curFound[r] < prevRects.size()) {
                prevUsed[curFound[r]] = true;
            }
        }

        for (size_t r = 0; (r < curRects.size()) && !untextured; r++) {
            if (curFound[r] < prevRects.size()) {
                continue;
            }

            const DrawCall &curCall = *curRects[r].call;
            int32_t nearest = INT32_MAX;
            for (size_t j = 0; j < prevRects.size(); j++) {
                const DrawCall &prevCall = *prevRects[j].call;
                if (((curOwn[r] < prevRects.size()) && (j != curOwn[r])) || prevUsed[j] || sameSize(curCall, prevCall) || !sameLook(curCall, prevCall) ||
                    !edgesFollow(curRects[r], prevRects[j]))
                {
                    continue;
                }

                const int32_t distance = abs(prevCall.gameRect.ulx - curCall.gameRect.ulx) + abs(prevCall.gameRect.uly - curCall.gameRect.uly) +
                    abs(prevCall.gameRect.lrx - curCall.gameRect.lrx) + abs(prevCall.gameRect.lry - curCall.gameRect.lry);
                if (distance < nearest) {
                    nearest = distance;
                    curFound[r] = j;
                }
            }

            if (curFound[r] < prevRects.size()) {
                prevUsed[curFound[r]] = true;
            }
        }

        thread_local std::vector<bool> curScaled;
        curScaled.assign(curRects.size(), false);
        for (size_t r = 0; (r < curRects.size()) && !untextured; r++) {
            if (curFound[r] < prevRects.size()) {
                continue;
            }

            const DrawCall &curCall = *curRects[r].call;
            int32_t nearest = INT32_MAX;
            for (size_t j = 0; j < prevRects.size(); j++) {
                const DrawCall &prevCall = *prevRects[j].call;
                const bool sameScale = (curCall.rectDsdx == prevCall.rectDsdx) && (curCall.rectDtdy == prevCall.rectDtdy);
                if (((curOwn[r] < prevRects.size()) && (j != curOwn[r])) || prevUsed[j] || sameScale || !movedLess(curCall, prevCall) ||
                    !sameTiles(curCall, prevCall) || !sameCorners(curRects[r], prevRects[j]))
                {
                    continue;
                }

                const int32_t distance = abs(prevCall.gameRect.ulx - curCall.gameRect.ulx) + abs(prevCall.gameRect.uly - curCall.gameRect.uly) +
                    abs(prevCall.gameRect.lrx - curCall.gameRect.lrx) + abs(prevCall.gameRect.lry - curCall.gameRect.lry);
                if (distance < nearest) {
                    nearest = distance;
                    curFound[r] = j;
                }
            }

            if (curFound[r] < prevRects.size()) {
                prevUsed[curFound[r]] = true;
                curScaled[r] = true;
            }
        }

        // By a rect's index, the texcoords its upper left moved by from the frame before when its texture slides by
        // them, for the strip that adjoins it with no pair of its own (below).
        struct RectSlide {
            float ds = 0.0f;
            float dt = 0.0f;
            bool slid = false;
        };

        thread_local std::vector<RectSlide> curSlides;
        curSlides.assign(curRects.size(), RectSlide());
        for (size_t r = 0; r < curRects.size(); r++) {
            const size_t found = curFound[r];
            if (found == prevRects.size()) {
                continue;
            }

            const RectCall &curRect = curRects[r];
            const DrawCall &curCall = *curRect.call;
            const DrawCall &prevCall = *prevRects[found].call;
            const float *curTc = &curData.triTcFloats[curRect.rawVertex * 2];
            const float *prevTc = &prevData.triTcFloats[prevRects[found].rawVertex * 2];
            const float ds = curTc[0] - prevTc[0];
            const float dt = curTc[1] - prevTc[1];
            const hlslpp::float4 edgeMoves = hlslpp::float4(float(prevCall.rect.ulx - curCall.rect.ulx), float(prevCall.rect.uly - curCall.rect.uly),
                float(prevCall.rect.lrx - curCall.rect.lrx), float(prevCall.rect.lry - curCall.rect.lry)) / 4.0f;
            auto setRectMoves = [&]() {
                if (workloadMap.rectMoves.size() <= curCall.callIndex) {
                    workloadMap.rectMoves.resize(curCall.callIndex + 1, hlslpp::float4(0.0f, 0.0f, 0.0f, 0.0f));
                }

                workloadMap.rectMoves[curCall.callIndex] = edgeMoves;
            };

            // One drawn at another scale with the same texels at its corners is drawn between there and here, its
            // texture stretched across it as the game stretches it.
            if (curScaled[r]) {
                setRectMoves();
                continue;
            }

            // A rect that changed size keeps its texels a pixel, so its texcoords moved by its upper left's. Its
            // texcoords follow the rect drawn, so this is the drawn rect's size. One with no texture has none to keep.
            const bool sameDrawnSize = ((curCall.rect.lrx - curCall.rect.ulx) == (prevCall.rect.lrx - prevCall.rect.ulx)) &&
                ((curCall.rect.lry - curCall.rect.uly) == (prevCall.rect.lry - prevCall.rect.uly));
            bool together = true;
            for (uint32_t v = 1; (v < 6) && together && sameDrawnSize && !untextured; v++) {
                together = (fabsf((curTc[v * 2 + 0] - prevTc[v * 2 + 0]) - ds) < 1.0e-3f) && (fabsf((curTc[v * 2 + 1] - prevTc[v * 2 + 1]) - dt) < 1.0e-3f);
            }

            if (!together) {
                continue;
            }

            // The texels the rect spans in each direction: its size in pixels times its texels a pixel.
            const float spanS = (float(curCall.rect.lrx - curCall.rect.ulx) / 4.0f) * (fabsf(float(curCall.rectDsdx)) / 1024.0f);
            const float spanT = (float(curCall.rect.lry - curCall.rect.uly) / 4.0f) * (fabsf(float(curCall.rectDtdy)) / 1024.0f);
            const bool cutS = cutAt(curRect, prevRects[found], false);
            const bool cutT = cutAt(curRect, prevRects[found], true);
            const int heldS = cutS ? heldEdge(curCall, prevCall, false) : 0;
            const int heldT = cutT ? heldEdge(curCall, prevCall, true) : 0;
            float stepS[8];
            float stepT[8];
            bool scrolled = false;
            bool slides = true;
            for (uint32_t t = 0; (t < curCall.tileCount) && slides; t++) {
                const interop::RDPTile &tile = curData.rdpTiles[curCall.tileIndex + t];
                stepS[t] = insidePeriod(ds * tile.shifts, tile.masks, tile.cms);
                stepT[t] = insidePeriod(dt * tile.shiftt, tile.maskt, tile.cmt);
                slides = (cutS || (stepS[t] == 0.0f) || (fabsf(stepS[t]) < (spanS * tile.shifts / 4.0f))) &&
                    (cutT || (stepT[t] == 0.0f) || (fabsf(stepT[t]) < (spanT * tile.shiftt / 4.0f)));
                scrolled = scrolled || (stepS[t] != 0.0f) || (stepT[t] != 0.0f);
            }

            const bool moved = (prevCall.rect.ulx != curCall.rect.ulx) || (prevCall.rect.uly != curCall.rect.uly) ||
                (prevCall.rect.lrx != curCall.rect.lrx) || (prevCall.rect.lry != curCall.rect.lry);
            if (!moved && (!slides || !scrolled)) {
                continue;
            }

            // A rect whose texture jumped (its texcoords moved a quarter of what it spans or more, not a scroll inside
            // it), where the frame before drew other rects with its look, is another picture of a texture they share,
            // not this one moved, cut or resized: a glyph of a font the game packs into one texture page with the rest
            // and draws by its texcoords (Excitebike 64's, its race's times and score one 64x64 page). As the race's
            // timer counted, a digit paired with the frame before's digit in its place, a glyph of another size, or
            // with one a cell along the line, and the frames between drew it stretched between the two sizes, slid
            // across its neighbours, or cut as a panorama's strip is (cutAt), the page's texels sliding through it and
            // the glyphs beside it in the page showing as columns. A step taken on an axis the rect is cut at the
            // scissor's edge it holds stays a cut: a strip of a texture several rects share, sliding off the screen.
            // So does one whose top edge moved, or of its size that moved along its row less than half of it: a sprite
            // among others that animates by stepping its texcoords across a strip of its frames as it drifts (Mario
            // Golf's sparkles in its credits), where a line of glyphs keeps its row.
            bool jumped = false;
            for (uint32_t t = 0; (t < curCall.tileCount) && !jumped && !untextured; t++) {
                const interop::RDPTile &tile = curData.rdpTiles[curCall.tileIndex + t];
                const float jumpS = fabsf(insidePeriod(ds * tile.shifts, tile.masks, tile.cms));
                const float jumpT = fabsf(insidePeriod(dt * tile.shiftt, tile.maskt, tile.cmt));
                jumped = (!(cutS && heldAtScissor(curCall, prevCall, false)) && (jumpS > 0.0f) && (jumpS >= (spanS * tile.shifts / 4.0f))) ||
                    (!(cutT && heldAtScissor(curCall, prevCall, true)) && (jumpT > 0.0f) && (jumpT >= (spanT * tile.shiftt / 4.0f)));
            }

            const bool movedHalf = (abs(prevCall.gameRect.ulx - curCall.gameRect.ulx) * 2 >= (curCall.gameRect.lrx - curCall.gameRect.ulx));
            const bool another = jumped && (prevCall.gameRect.uly == curCall.gameRect.uly) && (!sameSize(curCall, prevCall) || movedHalf);
            bool shared = false;
            for (size_t k = 0; (k < prevRects.size()) && another && !shared; k++) {
                shared = (k != found) && sameLook(curCall, *prevRects[k].call);
            }

            if (shared) {
                continue;
            }

            // A textured rect that moved along its row onto most of the place of a rect of that row the frame before
            // drew at its scale that nothing of this frame paired with, while another rect of this frame at that scale
            // took most of the place it left, in its row too, is another picture in each place: a line of glyphs whose
            // values changed, not a glyph moved along it. Excitebike 64 draws its race timer's glyphs page by page, so
            // as it went from 59.01 to 58.98 the tenths' 9 paired with the seconds' 9 as its own, a glyph and the point
            // to its left, and the frames between slid it across them. A line that slides pairs the glyphs in the
            // places its glyphs move onto, all used.
            auto sameScale = [](const DrawCall &a, const DrawCall &b) {
                return (a.rectDsdx == b.rectDsdx) && (a.rectDtdy == b.rectDtdy);
            };

            auto mostOf = [](const DrawCall &a, const DrawCall &b) {
                const int64_t w = std::min(a.gameRect.lrx, b.gameRect.lrx) - std::max(a.gameRect.ulx, b.gameRect.ulx);
                const int64_t h = std::min(a.gameRect.lry, b.gameRect.lry) - std::max(a.gameRect.uly, b.gameRect.uly);
                const int64_t area = int64_t(a.gameRect.lrx - a.gameRect.ulx) * int64_t(a.gameRect.lry - a.gameRect.uly);
                return (w > 0) && (h > 0) && ((w * h * 2) > area);
            };

            bool replaced = false;
            if (moved && !untextured && (prevCall.gameRect.uly == curCall.gameRect.uly)) {
                bool placeTaken = false;
                for (size_t k = 0; (k < prevRects.size()) && !placeTaken; k++) {
                    const DrawCall &placeCall = *prevRects[k].call;
                    placeTaken = (k != found) && !prevUsed[k] && (placeCall.gameRect.uly == curCall.gameRect.uly) && sameScale(curCall, placeCall) &&
                        mostOf(curCall, placeCall);
                }

                for (size_t c = 0; (c < curRects.size()) && placeTaken && !replaced; c++) {
                    const DrawCall &leftCall = *curRects[c].call;
                    replaced = (c != r) && (leftCall.gameRect.uly == prevCall.gameRect.uly) && sameScale(leftCall, prevCall) && mostOf(prevCall, leftCall);
                }
            }

            if (replaced) {
                continue;
            }

            // Whether its texture slides or jumps: a sprite the game moves and animates by stepping its texcoords
            // across a strip of its frames jumps a frame at a time, and moves on every game frame.
            if (moved) {
                if (workloadMap.rectMoves.size() <= curCall.callIndex) {
                    workloadMap.rectMoves.resize(curCall.callIndex + 1, hlslpp::float4(0.0f, 0.0f, 0.0f, 0.0f));
                }

                // An edge the game holds where it cuts the rect stays there in the frames between: one that came onto
                // the scissor's left the strip beyond it no rect in this frame, and drawn between where it was and the
                // scissor it showed a row of what lies behind (the clear color's sky, over Mario Golf's hills).
                workloadMap.rectMoves[curCall.callIndex] = hlslpp::float4(
                    (heldS == 1) ? 0.0f : float(prevCall.rect.ulx - curCall.rect.ulx), (heldT == 1) ? 0.0f : float(prevCall.rect.uly - curCall.rect.uly),
                    (heldS == 2) ? 0.0f : float(prevCall.rect.lrx - curCall.rect.lrx), (heldT == 2) ? 0.0f : float(prevCall.rect.lry - curCall.rect.lry)) / 4.0f;
            }

            // A rect cut at an edge it holds that changed size is drawn wider or narrower in the frames between, its
            // texcoords at its corners as this frame has them, which stretches its texture across it: Mario Golf
            // opens Course Select by moving Play Modes' halves out 12 pixels a side each game frame, each cut by the
            // screen's edge, and a half was drawn stretched toward that edge and past where the next game frame puts
            // it. So its texels are kept a pixel and move with the edge that moved, as the game moves them: each tile
            // takes the texels a texcoord its drawn size in the frame before asks for, and its corners are placed so
            // the texel at the moving edge is the one this frame has there, both interpolated like its corners. In
            // pixels, the drawn size W moved by dW, the upper left edge by dU and the moving edge by dM; a texcoord t
            // at the upper left and a scale of a texcoords a pixel give the frame before shifts * (W + dW) / W and its
            // corners 4 * shifts * (t * dW / W + (dM - dU) * a) further on. It takes the place of the step slide on
            // that axis, which it contains. An edge held where it came onto the scissor stays there (above), so the
            // rect's size changes between by its other edge's move, and its texels move with that edge.
            auto cutResize = [&](bool vertical, float &scale, float &corner) {
                const bool cut = vertical ? cutT : cutS;
                const int held = vertical ? heldT : heldS;
                const int32_t curUl = vertical ? curCall.rect.uly : curCall.rect.ulx;
                const int32_t curLr = vertical ? curCall.rect.lry : curCall.rect.lrx;
                const int32_t prevUl = vertical ? prevCall.rect.uly : prevCall.rect.ulx;
                const int32_t prevLr = vertical ? prevCall.rect.lry : prevCall.rect.lrx;
                const int32_t ulMove = (held == 1) ? 0 : (prevUl - curUl);
                const int32_t lrMove = (held == 2) ? 0 : (prevLr - curLr);
                if (!cut || (curLr <= curUl) || (ulMove == lrMove)) {
                    return false;
                }

                const float width = float(curLr - curUl) / 4.0f;
                const float dU = float(ulMove) / 4.0f;
                const float dW = float(lrMove - ulMove) / 4.0f;
                const float dM = (held == 1) ? (float(lrMove) / 4.0f) : dU;
                const float perPixel = float(vertical ? curCall.rectDtdy : curCall.rectDsdx) / 1024.0f;
                const float texcoord = curTc[vertical ? 1 : 0];
                scale = (width + dW) / width;
                corner = texcoord * dW / width + (dM - dU) * perPixel;
                return true;
            };

            float scaleS = 1.0f, cornerS = 0.0f, scaleT = 1.0f, cornerT = 0.0f;
            const bool resizedS = moved && cutResize(false, scaleS, cornerS);
            const bool resizedT = moved && cutResize(true, scaleT, cornerT);
            const bool stepped = slides && scrolled;
            if (!stepped && !resizedS && !resizedT) {
                continue;
            }

            // A tile's corners are in quarter texels, and the shader subtracts them from the texcoords it scaled.
            for (uint32_t t = 0; t < curCall.tileCount; t++) {
                const uint32_t tileIndex = curCall.tileIndex + t;
                const interop::RDPTile &tile = curData.rdpTiles[tileIndex];
                GameFrameMap::TileMap &tileMap = workloadMap.tiles[tileIndex];
                const float moveS = resizedS ? (tile.shifts * cornerS) : (stepped ? stepS[t] : 0.0f);
                const float moveT = resizedT ? (tile.shiftt * cornerT) : (stepped ? stepT[t] : 0.0f);
                tileMap.curUls = tile.uls;
                tileMap.curUlt = tile.ult;
                tileMap.curLrs = tile.lrs;
                tileMap.curLrt = tile.lrt;
                tileMap.prevUls = tile.uls + moveS * 4.0f;
                tileMap.prevUlt = tile.ult + moveT * 4.0f;
                tileMap.prevLrs = tile.lrs + moveS * 4.0f;
                tileMap.prevLrt = tile.lrt + moveT * 4.0f;
                tileMap.prevShiftsScale = scaleS;
                tileMap.prevShifttScale = scaleT;
                tileMap.mapped = true;
            }

            if (stepped) {
                curSlides[r] = { resizedS ? 0.0f : ds, resizedT ? 0.0f : dt, true };
            }

            tileInterpolationUsed = true;
        }

        // A strip of a panorama that the frame before drew nothing of, lying wholly past the screen's edge, has no rect
        // there to pair with: the game cut it to an empty rect, which the RDP drops. It is the one that has just come
        // in, cut at the edge as the panorama moves on to it, and left unslid it showed this frame's texels while the
        // strip it adjoins slid toward them from the frame before's, so in the frames between it stood aside of the
        // strips below by part of a step. One still unpaired that adjoins a rect whose texture slid, within a pixel,
        // across the same columns (or rows) with the same texcoord there, at the same scale and with the same tile
        // layout, is a strip of the same panorama, and its texture slides with that one's along the edge they share:
        // Mario Golf's hills, a column of strips a few rows high, each its own texture, that the game scrolls sideways
        // by their texcoords as the camera turns, a new strip coming in at the top as the camera pitches down.
        auto adjoins = [&](const RectCall &ra, const RectCall &rb, bool vertical) {
            const DrawCall &a = *ra.call;
            const DrawCall &b = *rb.call;
            const uint32_t along = vertical ? 0 : 1;
            const bool sameSpan = vertical ? ((a.gameRect.ulx == b.gameRect.ulx) && (a.gameRect.lrx == b.gameRect.lrx)) :
                ((a.gameRect.uly == b.gameRect.uly) && (a.gameRect.lry == b.gameRect.lry));
            const int32_t aUl = vertical ? a.gameRect.uly : a.gameRect.ulx;
            const int32_t aLr = vertical ? a.gameRect.lry : a.gameRect.lrx;
            const int32_t bUl = vertical ? b.gameRect.uly : b.gameRect.ulx;
            const int32_t bLr = vertical ? b.gameRect.lry : b.gameRect.lrx;
            if (!sameSpan || ((abs(bUl - aLr) > 4) && (abs(aUl - bLr) > 4)) || (a.rectDsdx != b.rectDsdx) || (a.rectDtdy != b.rectDtdy) ||
                (curData.triTcFloats[ra.rawVertex * 2 + along] != curData.triTcFloats[rb.rawVertex * 2 + along]) || (a.tileCount != b.tileCount))
            {
                return false;
            }

            for (uint32_t t = 0; t < a.tileCount; t++) {
                const interop::RDPTile &ta = curData.rdpTiles[a.tileIndex + t];
                const interop::RDPTile &tb = curData.rdpTiles[b.tileIndex + t];
                if ((ta.fmt != tb.fmt) || (ta.siz != tb.siz) || (ta.masks != tb.masks) || (ta.maskt != tb.maskt) || (ta.cms != tb.cms) ||
                    (ta.cmt != tb.cmt) || (ta.shifts != tb.shifts) || (ta.shiftt != tb.shiftt))
                {
                    return false;
                }
            }

            return true;
        };

        // A strip that slides so passes it on to the next unpaired one: at a faster pitch the strip under the new one
        // went from cut to whole, and paired with nothing either.
        bool passedOn = !untextured;
        while (passedOn) {
            passedOn = false;
            for (size_t r = 0; r < curRects.size(); r++) {
                if ((curFound[r] < prevRects.size()) || curSlides[r].slid) {
                    continue;
                }

                const DrawCall &curCall = *curRects[r].call;
                for (size_t q = 0; q < curRects.size(); q++) {
                    const RectSlide slide = curSlides[q];
                    const bool vertical = slide.slid && (slide.ds != 0.0f) && adjoins(curRects[r], curRects[q], true);
                    if (!vertical && !(slide.slid && (slide.dt != 0.0f) && adjoins(curRects[r], curRects[q], false))) {
                        continue;
                    }

                    for (uint32_t t = 0; t < curCall.tileCount; t++) {
                        const uint32_t tileIndex = curCall.tileIndex + t;
                        const interop::RDPTile &tile = curData.rdpTiles[tileIndex];
                        GameFrameMap::TileMap &tileMap = workloadMap.tiles[tileIndex];
                        const float moveS = vertical ? insidePeriod(slide.ds * tile.shifts, tile.masks, tile.cms) : 0.0f;
                        const float moveT = vertical ? 0.0f : insidePeriod(slide.dt * tile.shiftt, tile.maskt, tile.cmt);
                        tileMap.curUls = tile.uls;
                        tileMap.curUlt = tile.ult;
                        tileMap.curLrs = tile.lrs;
                        tileMap.curLrt = tile.lrt;
                        tileMap.prevUls = tile.uls + moveS * 4.0f;
                        tileMap.prevUlt = tile.ult + moveT * 4.0f;
                        tileMap.prevLrs = tile.lrs + moveS * 4.0f;
                        tileMap.prevLrt = tile.lrt + moveT * 4.0f;
                        tileMap.prevShiftsScale = 1.0f;
                        tileMap.prevShifttScale = 1.0f;
                        tileMap.mapped = true;
                    }

                    curSlides[r] = { vertical ? slide.ds : 0.0f, vertical ? 0.0f : slide.dt, true };
                    passedOn = true;
                    tileInterpolationUsed = true;
                    break;
                }
            }
        }
    }
#endif

    void GameFrame::match(RenderWorker *worker, WorkloadQueue &workloadQueue, const GameFrame &prevFrame, BufferUploader *velocityUploader, bool &velocityUploaderUsed, bool &tileInterpolationUsed, bool &lookAtInterpolationUsed) {
        tileInterpolationUsed = false;
        lookAtInterpolationUsed = false;
        matched = true;

        thread_local std::unordered_map<uint32_t, ModifiedBuffers> workloadsModified;
        workloadsModified.clear();

#if RT64_GAME_FRAME_WORKLOADS
        // The workloads of a frame that draw are paired with the frame before's in their order, and one that draws
        // nothing (a task of the game's that isn't a display list) is left out, so it can't shift the rest.
        thread_local std::vector<uint32_t> curDrawn;
        thread_local std::vector<uint32_t> prevDrawn;
        auto drawnWorkloads = [&](const std::vector<uint32_t> &frameWorkloads, std::vector<uint32_t> &drawn) {
            drawn.clear();
            for (uint32_t workloadIndex : frameWorkloads) {
                if (workloadQueue.workloads[workloadIndex].fbPairCount > 0) {
                    drawn.emplace_back(workloadIndex);
                }
            }
        };

        drawnWorkloads(workloads, curDrawn);
        drawnWorkloads(prevFrame.workloads, prevDrawn);
#else
        const std::vector<uint32_t> &curDrawn = workloads;
        const std::vector<uint32_t> &prevDrawn = prevFrame.workloads;
#endif
        thread_local std::vector<std::pair<uint32_t, uint32_t>> drawnPairs;
        drawnPairs.clear();
#if RT64_GAME_FRAME_WORKLOADS
        if (curDrawn.size() != prevDrawn.size()) {
            alignDrawnWorkloads(workloadQueue, curDrawn, prevDrawn, drawnPairs);
        }
        else
#endif
        {
            for (uint32_t w = 0; (w < curDrawn.size()) && (w < prevDrawn.size()); w++) {
                drawnPairs.emplace_back(curDrawn[w], prevDrawn[w]);
            }
        }

        for (const std::pair<uint32_t, uint32_t> &drawnPair : drawnPairs) {
            const uint32_t curWorkloadIndex = drawnPair.first;

            // We assume the workloads will be detected in the same order between frames.
            GameFrameMap::WorkloadMap &workloadMap = frameMap.workloads[curWorkloadIndex];
            workloadMap.prevWorkloadIndex = drawnPair.second;
            workloadMap.mapped = true;

            Workload &curWorkload = workloadQueue.workloads[curWorkloadIndex];
            const Workload &prevWorkload = workloadQueue.workloads[workloadMap.prevWorkloadIndex];
            workloadMap.viewProjections.clear();
            workloadMap.viewProjections.resize(curWorkload.drawData.viewProjTransforms.size());
            workloadMap.transforms.clear();
            workloadMap.transforms.resize(curWorkload.drawData.worldTransforms.size());
            workloadMap.tiles.clear();
            workloadMap.tiles.resize(curWorkload.drawData.rdpTiles.size());
            workloadMap.lookAt.clear();
            workloadMap.lookAt.resize(curWorkload.drawData.rspLookAt.size());
            workloadMap.prevTransformsMapped.clear();
            workloadMap.prevTransformsMapped.resize(prevWorkload.drawData.worldTransforms.size());
            workloadMap.prevTilesMapped.clear();
            workloadMap.prevTilesMapped.resize(prevWorkload.drawData.rdpTiles.size());
            workloadMap.prevLookAtMapped.clear();
            workloadMap.prevLookAtMapped.resize(prevWorkload.drawData.rspLookAt.size());

            buildTransformIdMap(curWorkload, curWorkload.transformIdMap, curWorkload.transformIgnoredIds);

            // Retrieve the matching maps for the current and previous workload.
            GameFrameMap::WorkloadMap &curWorkloadMap = frameMap.workloads[curWorkloadIndex];
            const GameFrameMap::WorkloadMap *prevWorkloadMap = nullptr;
            if (prevFrame.matched && prevFrame.frameMap.workloads[workloadMap.prevWorkloadIndex].mapped) {
                prevWorkloadMap = &prevFrame.frameMap.workloads[workloadMap.prevWorkloadIndex];
            }

            // Match the transforms linearly in the order they were submitted.
            ModifiedBuffers modifiedBuffers;
            auto curIt = curWorkload.transformIdMap.begin();
            auto prevIt = prevWorkload.transformIdMap.begin();
            bool modifiedVelocityBuffer = false;
            while ((curIt != curWorkload.transformIdMap.end()) && (prevIt != prevWorkload.transformIdMap.end())) {
                if (curIt->first < prevIt->first) {
                    curIt++;
                }
                else if (curIt->first > prevIt->first) {
                    prevIt++;
                }
                else {
                    matchTransform(curWorkload, prevWorkload, curWorkloadMap, prevWorkloadMap, curIt->second, prevIt->second, modifiedBuffers);
                    curIt++;
                    prevIt++;
                }
            }

            if (!modifiedBuffers.empty()) {
                workloadsModified[curWorkloadIndex].merge(modifiedBuffers);
            }

            // Any transforms tagged with the empty ID will be instantly marked as used and skipped.
            for (uint32_t curIt : curWorkload.transformIgnoredIds) {
                GameFrameMap::TransformMap &curTransformMap = curWorkloadMap.transforms[curIt];
                curTransformMap.rigidBody = RigidBody();
                curTransformMap.prevTransformIndex = 0;
                curTransformMap.mapped = false;
            }
        }

        thread_local std::vector<MatchCandidate> matchCandidates;
        thread_local std::vector<bool> curScenesMatched;
        thread_local std::vector<bool> prevScenesMatched;
        auto matchScenes = [&](const std::vector<GameScene> &curScenes, const std::vector<GameScene> &prevScenes) {
            // Find the scenes that are the closest match possible.
            matchCandidates.clear();
            for (uint32_t i = 0; i < curScenes.size(); i++) {
                const GameIndices::Projection curFirstProj = curScenes[i].projections[0];
                const Workload &curWorkload = workloadQueue.workloads[curFirstProj.workloadIndex];
                const FramebufferPair &curFbPair = curWorkload.fbPairs[curFirstProj.fbPairIndex];
                const Projection &curProj = curFbPair.projections[curFirstProj.projectionIndex];
                const hlslpp::float4x4 &curViewTransform = curWorkload.drawData.viewTransforms[curProj.transformsIndex];
                const hlslpp::float4x4 &curProjTransform = curWorkload.drawData.projTransforms[curProj.transformsIndex];
                for (uint32_t j = 0; j < prevScenes.size(); j++) {
                    const GameIndices::Projection prevFirstProj = prevScenes[j].projections[0];
#if RT64_GAME_FRAME_WORKLOADS
                    // Only with a scene of the workload this one's was paired with: matchScene indexes the frame before's
                    // transforms, tiles and look ats through maps sized for that workload.
                    const GameFrameMap::WorkloadMap &curWorkloadMap = frameMap.workloads[curFirstProj.workloadIndex];
                    if (!curWorkloadMap.mapped || (curWorkloadMap.prevWorkloadIndex != prevFirstProj.workloadIndex)) {
                        continue;
                    }
#endif
                    const Workload &prevWorkload = workloadQueue.workloads[prevFirstProj.workloadIndex];
                    const FramebufferPair &prevFbPair = prevWorkload.fbPairs[prevFirstProj.fbPairIndex];
                    const Projection &prevProj = prevFbPair.projections[prevFirstProj.projectionIndex];
                    const hlslpp::float4x4 &prevViewTransform = prevWorkload.drawData.viewTransforms[prevProj.transformsIndex];
                    const hlslpp::float4x4 &prevProjTransform = prevWorkload.drawData.projTransforms[prevProj.transformsIndex];
                    matchCandidates.emplace_back(i, j, matrixDifference(curViewTransform, prevViewTransform) + matrixDifference(curProjTransform, prevProjTransform));
                }
            }

            curScenesMatched.clear();
            prevScenesMatched.clear();
            curScenesMatched.resize(curScenes.size());
            prevScenesMatched.resize(prevScenes.size());

            // A camera that isn't finite (one a game draws a shadow into a texture with can have a projection of NaNs)
            // differs by NaN from every other, and a NaN compares false both ways: it broke the sort below, and the
            // greedy pass paired a scene with that camera's while its own match was there, matching its transforms
            // against the other scene's. A scene with a camera that isn't finite pairs only with another such scene,
            // in order, never with one that is.
            auto isCameraFinite = [&](const GameScene &scene) {
                const GameIndices::Projection &firstProj = scene.projections[0];
                const Workload &workload = workloadQueue.workloads[firstProj.workloadIndex];
                const Projection &proj = workload.fbPairs[firstProj.fbPairIndex].projections[firstProj.projectionIndex];
                const hlslpp::float4x4 &viewTransform = workload.drawData.viewTransforms[proj.transformsIndex];
                const hlslpp::float4x4 &projTransform = workload.drawData.projTransforms[proj.transformsIndex];
                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) {
                        if (!std::isfinite(float(viewTransform[i][j])) || !std::isfinite(float(projTransform[i][j]))) {
                            return false;
                        }
                    }
                }

                return true;
            };

            auto pairsAcross = [&](const MatchCandidate &candidate) {
                return isCameraFinite(curScenes[candidate.curIndex]) != isCameraFinite(prevScenes[candidate.prevIndex]);
            };

            matchCandidates.erase(std::remove_if(matchCandidates.begin(), matchCandidates.end(), pairsAcross), matchCandidates.end());
            for (MatchCandidate &candidate : matchCandidates) {
                if (!std::isfinite(candidate.difference)) {
                    candidate.difference = FLT_MAX;
                }
            }

            std::stable_sort(matchCandidates.begin(), matchCandidates.end());
            for (const MatchCandidate &candidate : matchCandidates) {
                if (curScenesMatched[candidate.curIndex]) {
                    continue;
                }

                if (prevScenesMatched[candidate.prevIndex]) {
                    continue;
                }

                matchScene(workloadQueue, prevFrame, curScenes[candidate.curIndex], prevScenes[candidate.prevIndex], workloadsModified, tileInterpolationUsed, lookAtInterpolationUsed);
                curScenesMatched[candidate.curIndex] = true;
                prevScenesMatched[candidate.prevIndex] = true;
            }
        };

        matchScenes(perspectiveScenes, prevFrame.perspectiveScenes);
        matchScenes(orthographicScenes, prevFrame.orthographicScenes);

        if (!workloadsModified.empty()) {
            thread_local std::vector<BufferUploader::Upload> uploads;
            uploads.clear();

            for (auto &it : workloadsModified) {
                Workload &workload = workloadQueue.workloads[it.first];
                if (it.second.positionVelocity) {
                    uploads.emplace_back(BufferUploader::Upload{ workload.drawData.velFloats.data(), { 0, workload.drawData.velFloats.size() }, sizeof(float), RenderBufferFlag::FORMATTED, { RenderFormat::R32_FLOAT }, &workload.drawBuffers.velocityBuffer });
                }

                if (it.second.texcoordVelocity) {
                    uploads.emplace_back(BufferUploader::Upload{ workload.drawData.tcVelFloats.data(), { 0, workload.drawData.tcVelFloats.size() }, sizeof(float), RenderBufferFlag::FORMATTED, { RenderFormat::R32_FLOAT }, &workload.drawBuffers.texcoordVelocityBuffer });
                }
            }

            velocityUploader->submit(worker, uploads);
            velocityUploaderUsed = true;
        }
        else {
            velocityUploaderUsed = false;
        }

        // The rects pair last: none of their tiles is one matchScene() maps, and tileInterpolationUsed is read once
        // match() returns.
#if RT64_INTERPOLATE_RECT_SCROLLS
        for (const std::pair<uint32_t, uint32_t> &drawnPair : drawnPairs) {
            matchRectScrolls(workloadQueue.workloads[drawnPair.first], workloadQueue.workloads[drawnPair.second], frameMap.workloads[drawnPair.first], tileInterpolationUsed, false);
            matchRectScrolls(workloadQueue.workloads[drawnPair.first], workloadQueue.workloads[drawnPair.second], frameMap.workloads[drawnPair.first], tileInterpolationUsed, true);
        }
#endif
    }

    void GameFrame::matchScene(WorkloadQueue &workloadQueue, const GameFrame &prevFrame, const GameScene &curScene, const GameScene &prevScene, std::unordered_map<uint32_t, ModifiedBuffers> &workloadsModified, bool &tileInterpolationUsed, bool &lookAtInterpolationUsed) {
        if (curScene.projections.empty() || prevScene.projections.empty()) {
            return;
        }

        // Sorted vectors rather than multimaps and sets: they are rebuilt for every scene of every frame, and the
        // node allocations were most of this thread's time. Sorted the same way, so they are visited in the same order.
        thread_local std::vector<std::pair<uint64_t, GameCallMap>> curCallHashMap;
        thread_local std::vector<std::pair<uint64_t, GameCallMap>> prevCallHashMap;
        curCallHashMap.clear();
        prevCallHashMap.clear();

        // Pick the first projection for reference of the scene.
        const GameIndices::Projection &firstCurProjIndices = curScene.projections[0];
        const GameIndices::Projection &firstPrevProjIndices = prevScene.projections[0];
        GameFrameMap::WorkloadMap &firstCurWorkloadMap = frameMap.workloads[firstCurProjIndices.workloadIndex];
        Workload &firstCurWorkload = workloadQueue.workloads[firstCurProjIndices.workloadIndex];
        const FramebufferPair &firstCurFbPair = firstCurWorkload.fbPairs[firstCurProjIndices.fbPairIndex];
        const Projection &firstCurProj = firstCurFbPair.projections[firstCurProjIndices.projectionIndex];
        const Workload &firstPrevWorkload = workloadQueue.workloads[firstPrevProjIndices.workloadIndex];
        const FramebufferPair &firstPrevFbPair = firstPrevWorkload.fbPairs[firstPrevProjIndices.fbPairIndex];
        const Projection &firstPrevProj = firstPrevFbPair.projections[firstPrevProjIndices.projectionIndex];
        const GameFrameMap::WorkloadMap *firstPrevWorkloadMap = nullptr;
        if (prevFrame.matched && prevFrame.frameMap.workloads[firstPrevProjIndices.workloadIndex].mapped) {
            firstPrevWorkloadMap = &prevFrame.frameMap.workloads[firstPrevProjIndices.workloadIndex];
        }

#if RT64_SKIP_CAMERA_CUTS
        CameraMove worldStepCamera;
        float worldStepPrevEyeStep = -1.0f;
        uint32_t worldStepViewProjIndex = UINT32_MAX;
#endif

        // Build a multimap with all potential compatibilities between draw calls in the projections.
        uint32_t mappedViewProjIndex = UINT32_MAX;
        for (uint32_t p = 0; p < curScene.projections.size(); p++) {
            const GameIndices::Projection &curProjIndices = curScene.projections[p];
            Workload &curWorkload = workloadQueue.workloads[curProjIndices.workloadIndex];
            const FramebufferPair &curFbPair = curWorkload.fbPairs[curProjIndices.fbPairIndex];
            const Projection &curProj = curFbPair.projections[curProjIndices.projectionIndex];
            buildCallHashMap(p, curWorkload, curProj, curCallHashMap);

            // Projection for the entire scene has been matched already, copy the mapping.
            if (mappedViewProjIndex < UINT32_MAX) {
                firstCurWorkloadMap.viewProjections[curProj.transformsIndex] = firstCurWorkloadMap.viewProjections[mappedViewProjIndex];
            }

            // Can't map to a previous projection.
            if (p >= prevScene.projections.size()) {
                continue;
            }

            if (mappedViewProjIndex == UINT32_MAX) {
                GameFrameMap::ViewProjectionMap &viewProjMap = firstCurWorkloadMap.viewProjections[curProj.transformsIndex];
                if (viewProjMap.mapped) {
                    mappedViewProjIndex = curProj.transformsIndex;
                    continue;
                }

                const GameIndices::Projection &prevProjIndices = prevScene.projections[p];
                const Workload &prevWorkload = workloadQueue.workloads[prevProjIndices.workloadIndex];
                const FramebufferPair &prevFbPair = prevWorkload.fbPairs[prevProjIndices.fbPairIndex];
                const Projection &prevProj = prevFbPair.projections[prevProjIndices.projectionIndex];
                const hlslpp::float4x4 &curView = curWorkload.drawData.viewTransforms[curProj.transformsIndex];
                const hlslpp::float4x4 &prevView = prevWorkload.drawData.viewTransforms[prevProj.transformsIndex];
                const uint32_t curProjGroupIndex = curWorkload.drawData.viewProjTransformGroups[curProj.transformsIndex];
                const uint32_t prevProjGroupIndex = prevWorkload.drawData.viewProjTransformGroups[prevProj.transformsIndex];
                const TransformGroup &curProjGroup = curWorkload.drawData.transformGroups[curProjGroupIndex];
                const TransformGroup &prevProjGroup = prevWorkload.drawData.transformGroups[prevProjGroupIndex];

                // Cameras usually look better with simple interpolation, so default decomposition to off.
                bool projectionDecompose = false;
                uint8_t projectionLinearComponent = G_EX_COMPONENT_INTERPOLATE;
                uint8_t projectionAngularComponent = G_EX_COMPONENT_INTERPOLATE;
                uint8_t projectionScaleComponent = G_EX_COMPONENT_INTERPOLATE;
                uint8_t projectionSkewComponent = G_EX_COMPONENT_INTERPOLATE;
                uint8_t projectionPerspectiveComponent = G_EX_COMPONENT_INTERPOLATE;
                if ((curProjGroup.matrixId != G_EX_ID_IGNORE) && (curProjGroup.matrixId != G_EX_ID_AUTO)) {
                    projectionDecompose = curProjGroup.decompose;
                    projectionLinearComponent = curProjGroup.positionInterpolation;
                    projectionAngularComponent = curProjGroup.rotationInterpolation;
                    projectionScaleComponent = curProjGroup.scaleInterpolation;
                    projectionSkewComponent = curProjGroup.skewInterpolation;
                    projectionPerspectiveComponent = curProjGroup.perspectiveInterpolation;
                    viewProjMap.mapped = (curProjGroup.matrixId == prevProjGroup.matrixId);
                }
                else {
                    viewProjMap.mapped = (curProjGroup.matrixId == G_EX_ID_AUTO);
#               if RT64_SKIP_CAMERA_CUTS
                    // A camera the game gives no matrix group of its own that cut to another shot pairs with nothing,
                    // and neither does anything its scene draws: the frames between draw the new frame as it is, where
                    // a cut drawn halfway put the world between two places (isCameraCut()).
                    if (viewProjMap.mapped && (curProj.type == Projection::Type::Perspective) && (prevProj.type == Projection::Type::Perspective)) {
                        float prevEyeStep = -1.0f;
                        if (firstPrevWorkloadMap != nullptr) {
                            prevEyeStep = firstPrevWorkloadMap->viewProjections[prevProj.transformsIndex].eyeStep;
                        }

                        const hlslpp::float4x4 &curProjection = curWorkload.drawData.projTransforms[curProj.transformsIndex];
                        CameraMove move;
                        viewProjMap.eyeStep = -1.0f;
                        const hlslpp::float4x4 &curViewProj = curWorkload.drawData.viewProjTransforms[curProj.transformsIndex];
                        const hlslpp::float4x4 &prevViewProj = prevWorkload.drawData.viewProjTransforms[prevProj.transformsIndex];
                        if (measureCameraMove(curViewProj, prevViewProj, curView, curProjection, prevView, move)) {
                            viewProjMap.eyeStep = move.step;
                            if (isCameraCut(move.turnCosine, move.step, prevEyeStep)) {
                                viewProjMap.mapped = false;
                                viewProjMap.rigidBody = RigidBody();
                                return;
                            }

                            // A camera whose eye the view leaves at the world's origin: its step is read from the
                            // world's transforms once they are paired, and judged then.
                            if (move.step < 0.0f) {
                                worldStepCamera = move;
                                worldStepPrevEyeStep = prevEyeStep;
                                worldStepViewProjIndex = curProj.transformsIndex;
                            }
                        }
                    }
#               endif
                }

                if (viewProjMap.mapped) {
                    if (firstPrevWorkloadMap != nullptr) {
                        const GameFrameMap::ViewProjectionMap &prevViewProjMap = firstPrevWorkloadMap->viewProjections[prevProj.transformsIndex];
                        viewProjMap.rigidBody = prevViewProjMap.rigidBody;
                    }

                    viewProjMap.rigidBody.updateLinear(prevView, curView, projectionLinearComponent);
                    viewProjMap.rigidBody.updateAngular(prevView, curView, projectionAngularComponent, projectionScaleComponent, projectionSkewComponent);
                    viewProjMap.rigidBody.updateDecomposition(curView, projectionDecompose);
                    viewProjMap.prevTransformIndex = prevProj.transformsIndex;
                }
                else {
                    viewProjMap.rigidBody = RigidBody();
                }

                if (viewProjMap.mapped) {
                    mappedViewProjIndex = curProj.transformsIndex;
                }
            }
        }

        for (uint32_t p = 0; p < prevScene.projections.size(); p++) {
            const GameIndices::Projection &prevProjIndices = prevScene.projections[p];
            const Workload &prevWorkload = workloadQueue.workloads[prevProjIndices.workloadIndex];
            const FramebufferPair &prevFbPair = prevWorkload.fbPairs[prevProjIndices.fbPairIndex];
            const Projection &prevProj = prevFbPair.projections[prevProjIndices.projectionIndex];
            buildCallHashMap(p, prevWorkload, prevProj, prevCallHashMap);
        }

        // As a multimap orders them: by hash, and calls with the same hash in the order they were added.
        auto byHash = [](const std::pair<uint64_t, GameCallMap> &lhs, const std::pair<uint64_t, GameCallMap> &rhs) {
            return lhs.first < rhs.first;
        };
        std::stable_sort(curCallHashMap.begin(), curCallHashMap.end(), byHash);
        std::stable_sort(prevCallHashMap.begin(), prevCallHashMap.end(), byHash);

        // FIXME: Transform set needs to be done per unique workload detected.
        thread_local std::vector<IndexPair> transformCheckSet;
        thread_local std::vector<IndexPair> tileCheckSet;
        thread_local std::vector<IndexPair> lookAtCheckSet;
        transformCheckSet.clear();
        tileCheckSet.clear();
        lookAtCheckSet.clear();

        // Fill the sets with every combination of transforms, tiles and look ats the calls of a hash could match with the
        // previous frame's calls of the same hash, a hash's calls at a time. A pair of calls adds the pairs of their
        // matrices (from their workloads, at the same place in each call's range), of their tiles and of their look
        // ats, so it adds only what each call's own are: calls with the same range of matrices add the same transform
        // pairs, and those with no tiles to interpolate or no texture generation add none of those. A group of many
        // alike calls cost its square: Banjo-Tooie's levels drew up to 398 calls of one hash, a frame of 1,000 calls
        // made 167,000 pairs of calls for 200-1,800 pairs of transforms, and the matching took 18-23 ms of each game
        // frame on a phone, 13 ms of it making the pairs. So each side's distinct ranges are paired, and only the calls
        // that can add a tile or look at pair are, each call's lookups made once. The sets are sorted and their repeats
        // dropped after, so they hold what pairing every two calls put in them.
        struct CallMatrices {
            uint32_t workloadIndex;
            uint32_t minWorldMatrix;
            uint32_t maxWorldMatrix;

            bool operator<(const CallMatrices &o) const {
                if (workloadIndex != o.workloadIndex) {
                    return workloadIndex < o.workloadIndex;
                }

                return (minWorldMatrix != o.minWorldMatrix) ? (minWorldMatrix < o.minWorldMatrix) : (maxWorldMatrix < o.maxWorldMatrix);
            }

            bool operator==(const CallMatrices &o) const {
                return (workloadIndex == o.workloadIndex) && (minWorldMatrix == o.minWorldMatrix) && (maxWorldMatrix == o.maxWorldMatrix);
            }
        };

        struct CallTiles {
            const Workload *workload;
            uint32_t tileIndex;
            uint32_t tileCount;
            bool doTileMatching;
        };

        using CallHashIterator = std::vector<std::pair<uint64_t, GameCallMap>>::const_iterator;
        thread_local std::vector<CallMatrices> curMatrices;
        thread_local std::vector<CallMatrices> prevMatrices;
        thread_local std::vector<CallTiles> curTiles;
        thread_local std::vector<CallTiles> prevTiles;
        thread_local std::vector<uint32_t> curLookAts;
        thread_local std::vector<uint32_t> prevLookAts;
        auto collectCalls = [&](const GameScene &scene, CallHashIterator begin, CallHashIterator end, std::vector<CallMatrices> &matrices,
            std::vector<CallTiles> &tiles, std::vector<uint32_t> &lookAts)
        {
            matrices.clear();
            tiles.clear();
            lookAts.clear();
            for (CallHashIterator it = begin; it != end; it++) {
                const GameCallMap &callMap = it->second;
                const GameIndices::Projection &projIndices = scene.projections[callMap.sceneProjIndex];
                const Workload &workload = workloadQueue.workloads[projIndices.workloadIndex];
                const FramebufferPair &fbPair = workload.fbPairs[projIndices.fbPairIndex];
                const Projection &proj = fbPair.projections[projIndices.projectionIndex];
                const GameCall &call = proj.gameCalls[callMap.callIndex];
                if (callMap.doTransformMatching) {
                    matrices.push_back({ projIndices.workloadIndex, call.callDesc.minWorldMatrix, call.callDesc.maxWorldMatrix });
                }

                if (callMap.doTileInterpolation && (call.callDesc.tileCount > 0)) {
                    tiles.push_back({ &workload, call.callDesc.tileIndex, call.callDesc.tileCount, bool(callMap.doTileMatching) });
                }

                // FIXME: We assume the same look at is used throughout the entire draw call, so we only check the first vertex.
                const uint32_t textureGenMask = G_LIGHTING | G_TEXTURE_GEN;
                if ((call.callDesc.geometryMode & textureGenMask) == textureGenMask) {
                    const uint32_t vertexIndex = workload.drawData.faceIndices[call.meshDesc.faceIndicesStart];
                    lookAts.push_back(workload.drawData.lookAtIndices[vertexIndex] >> RSP_LOOKAT_INDEX_SHIFT);
                }
            }

            std::sort(matrices.begin(), matrices.end());
            matrices.erase(std::unique(matrices.begin(), matrices.end()), matrices.end());
            std::sort(lookAts.begin(), lookAts.end());
            lookAts.erase(std::unique(lookAts.begin(), lookAts.end()), lookAts.end());
        };

        for (CallHashIterator curBegin = curCallHashMap.cbegin(); curBegin != curCallHashMap.cend();) {
            CallHashIterator curEnd = curBegin;
            while ((curEnd != curCallHashMap.cend()) && (curEnd->first == curBegin->first)) {
                curEnd++;
            }

            auto prevRange = std::equal_range(prevCallHashMap.cbegin(), prevCallHashMap.cend(), *curBegin, byHash);
            if (prevRange.first != prevRange.second) {
                collectCalls(curScene, curBegin, curEnd, curMatrices, curTiles, curLookAts);
                collectCalls(prevScene, prevRange.first, prevRange.second, prevMatrices, prevTiles, prevLookAts);
                for (const CallMatrices &cur : curMatrices) {
                    const Workload &curWorkload = workloadQueue.workloads[cur.workloadIndex];
                    for (const CallMatrices &prev : prevMatrices) {
                        if ((cur.maxWorldMatrix - cur.minWorldMatrix) != (prev.maxWorldMatrix - prev.minWorldMatrix)) {
                            continue;
                        }

                        const Workload &prevWorkload = workloadQueue.workloads[prev.workloadIndex];
                        const uint32_t worldMatrixCount = (cur.maxWorldMatrix - cur.minWorldMatrix) + 1;
                        for (uint32_t w = 0; w < worldMatrixCount; w++) {
                            const uint32_t curWorldMatrix = cur.minWorldMatrix + w;
                            const uint32_t curGroupIndex = curWorkload.drawData.worldTransformGroups[curWorldMatrix];
                            const TransformGroup &curGroup = curWorkload.drawData.transformGroups[curGroupIndex];
                            const uint32_t prevWorldMatrix = prev.minWorldMatrix + w;
                            const uint32_t prevGroupIndex = prevWorkload.drawData.worldTransformGroups[prevWorldMatrix];
                            const TransformGroup &prevGroup = prevWorkload.drawData.transformGroups[prevGroupIndex];
                            if ((curGroup.matrixId == prevGroup.matrixId) && ((curGroup.matrixId == G_EX_ID_AUTO) || ((curGroup.matrixId != G_EX_ID_IGNORE) && (curGroup.ordering == G_EX_ORDER_AUTO)))) {
                                transformCheckSet.emplace_back(curWorldMatrix, prevWorldMatrix);
                            }
                        }
                    }
                }

                for (const CallTiles &cur : curTiles) {
                    for (const CallTiles &prev : prevTiles) {
                        if (cur.tileCount != prev.tileCount) {
                            continue;
                        }

                        const bool doTileMatching = cur.doTileMatching && prev.doTileMatching;
                        for (uint32_t t = 0; t < cur.tileCount; t++) {
                            const DrawCallTile &curCallTile = cur.workload->drawData.callTiles[cur.tileIndex + t];
                            const DrawCallTile &prevCallTile = prev.workload->drawData.callTiles[prev.tileIndex + t];
                            if (doTileMatching && (curCallTile.tmemHashOrID != prevCallTile.tmemHashOrID)) {
                                continue;
                            }

                            tileCheckSet.emplace_back(cur.tileIndex + t, prev.tileIndex + t);
                        }
                    }
                }

                for (uint32_t curLookAt : curLookAts) {
                    for (uint32_t prevLookAt : prevLookAts) {
                        lookAtCheckSet.emplace_back(curLookAt, prevLookAt);
                    }
                }
            }

            curBegin = curEnd;
        }

        // Sorted and without repeats, as the sets were.
        for (std::vector<IndexPair> *checkSet : { &transformCheckSet, &tileCheckSet, &lookAtCheckSet }) {
            sortUniqueIndexPairs(*checkSet);
        }

        // Compute all the differences between transforms and insert them into a vector that will be sorted according to the differences.
        thread_local std::vector<MatchCandidate> matchCandidates;
        matchCandidates.clear();

        const RigidBody *prevRigidBody;
        const hlslpp::float4x4 &firstCurViewProj = firstCurWorkload.drawData.viewProjTransforms[firstCurProj.transformsIndex];
        const hlslpp::float4x4 &firstPrevViewProj = firstPrevWorkload.drawData.viewProjTransforms[firstPrevProj.transformsIndex];
        for (const IndexPair &indices : transformCheckSet) {
            const hlslpp::float4x4 &curTransform = firstCurWorkload.drawData.worldTransforms[indices.first];
            const hlslpp::float4x4 &prevTransform = firstPrevWorkload.drawData.worldTransforms[indices.second];
            prevRigidBody = (firstPrevWorkloadMap != nullptr) ? &firstPrevWorkloadMap->transforms[indices.second].rigidBody : nullptr;

            TransformMatchResult matchResult = computeTransformMatch(curTransform, firstCurViewProj, prevTransform, firstPrevViewProj, prevRigidBody);
            if (matchResult.valid) {
                matchCandidates.emplace_back(indices.first, indices.second, matchResult.computeDifference());
            }
        }

#if RT64_SKIP_CAMERA_CUTS
        // A camera whose eye the view leaves at the world's origin, judged by the step its world made (worldEyeStep()).
        // A cut pairs nothing of the scene, as one the view shows does.
        if (worldStepViewProjIndex != UINT32_MAX) {
            GameFrameMap::ViewProjectionMap &viewProjMap = firstCurWorkloadMap.viewProjections[worldStepViewProjIndex];
            viewProjMap.eyeStep = worldEyeStep(matchCandidates, firstCurWorkload.drawData, firstPrevWorkload.drawData, worldStepCamera.rotation, worldStepCamera.nearDistance);
            if ((viewProjMap.eyeStep >= 0.0f) && isCameraCut(worldStepCamera.turnCosine, viewProjMap.eyeStep, worldStepPrevEyeStep)) {
                for (const GameIndices::Projection &curProjIndices : curScene.projections) {
                    const Workload &curWorkload = workloadQueue.workloads[curProjIndices.workloadIndex];
                    const Projection &curProj = curWorkload.fbPairs[curProjIndices.fbPairIndex].projections[curProjIndices.projectionIndex];
                    GameFrameMap::ViewProjectionMap &curViewProjMap = firstCurWorkloadMap.viewProjections[curProj.transformsIndex];
                    curViewProjMap.mapped = false;
                    curViewProjMap.rigidBody = RigidBody();
                }

                return;
            }
        }
#endif

#if RT64_SKIP_PLACE_SWAPS
        // Nor a transform with one of the frame before that drew at another place, when each frame drew something at
        // both places. Both places then held still, and the pair trades one object for another: a game that draws the
        // same model at many places can draw them in another order or with another call each frame (a level of detail
        // for the distance, the ones in view first), so the same call's transforms are paired across objects, and the
        // frames between drew each halfway to another's place. Mario Golf's flyover of a hole drew the course's trees
        // out on the fairway every other frame, and its trees jumped about as a shot was aimed, a fifth of a frame's
        // trees paired so. Left unpaired, each is drawn at its own place between the frames, as it stood. What moves
        // into the very place another left in the same frame is left unpaired too, and steps for a frame.
        //
        // Nor a transform whose vertices are other than those of the frame before's, unless that one's were changing
        // then (paired with other vertices in the frame before that): a mesh that held still or had just come, and has
        // other vertices now, is another mesh the game drew with the same call, not the same one moving. Turok's
        // levels are made of blocks, each a mesh of its own that never changes, and as one came into view, or took
        // another level of detail and so another call, it was paired with one that left, and drawn partway from its
        // place in the frames between: a slab of cliff over the sky, a hole at the picture's edge where the background
        // showed instead of a wall. Turok puts its camera in the world matrices in places, so each moves as the camera
        // turns, and only the meshes tell one block from another. A mesh that held still or had just come and then changes,
        // and what it is drawn with, steps for a frame.
        if (!matchCandidates.empty()) {
            thread_local std::vector<TransformPlace> curPlaces;
            thread_local std::vector<TransformPlace> prevPlaces;
            thread_local std::vector<uint64_t> curMeshes;
            thread_local std::vector<uint64_t> prevMeshes;
            collectTransformPlaces(firstCurWorkload, curPlaces);
            collectTransformPlaces(firstPrevWorkload, prevPlaces);
            collectTransformMeshes(firstCurWorkload, curMeshes);
            collectTransformMeshes(firstPrevWorkload, prevMeshes);
            auto tradesPlaces = [&](const MatchCandidate &candidate) {
                if ((firstPrevWorkloadMap != nullptr) && (candidate.curIndex < curMeshes.size()) && (candidate.prevIndex < prevMeshes.size())) {
                    const GameFrameMap::TransformMap &prevMap = firstPrevWorkloadMap->transforms[candidate.prevIndex];
                    if ((!prevMap.mapped || prevMap.verticesStill) && (curMeshes[candidate.curIndex] != prevMeshes[candidate.prevIndex])) {
                        return true;
                    }
                }

                const TransformPlace curPlace = transformPlace(firstCurWorkload.drawData.worldTransforms[candidate.curIndex]);
                const TransformPlace prevPlace = transformPlace(firstPrevWorkload.drawData.worldTransforms[candidate.prevIndex]);
                return !(curPlace == prevPlace) && std::binary_search(prevPlaces.begin(), prevPlaces.end(), curPlace) && std::binary_search(curPlaces.begin(), curPlaces.end(), prevPlace);
            };

            matchCandidates.erase(std::remove_if(matchCandidates.begin(), matchCandidates.end(), tradesPlaces), matchCandidates.end());
        }
#endif

        ModifiedBuffers modifiedBuffers;
        std::stable_sort(matchCandidates.begin(), matchCandidates.end());
        for (const MatchCandidate candidate : matchCandidates) {
            if (firstCurWorkloadMap.transforms[candidate.curIndex].mapped) {
                continue;
            }

            if (firstCurWorkloadMap.prevTransformsMapped[candidate.prevIndex]) {
                continue;
            }

            matchTransform(firstCurWorkload, firstPrevWorkload, firstCurWorkloadMap, firstPrevWorkloadMap, candidate.curIndex, candidate.prevIndex, modifiedBuffers);
        }

#if RT64_MOVE_UNPAIRED_WITH_CAMERA
        // A transform no pair was found for, drawn in the frames between as holding still in the world. A game that puts
        // its camera in each object's matrix moves everything that holds still by the camera's step, so most of the
        // scene's paired transforms agree on one step from their matrix to their pair's, and a transform with no pair
        // is drawn from its matrix moved back by that step. One a game draws with another call than the frame before's
        // as it nears (another render mode past the fog's start, another level of detail), or that comes into view, then
        // moves with its neighbours instead of standing at its own place while they move, with the gaps between them
        // open. Excitebike 64 draws each section of a track with a matrix of its own that holds the camera, fogged under
        // one render mode past a distance and plain under another nearer: a section that crossed that distance, and one
        // that came into view at the picture's side, stood still in the frames between, and the background colour showed
        // between it and the next in a light line across the track and up the canyon walls, for a frame at a time. A game
        // whose camera is in the projection leaves what holds still at the same matrix, so the step is none, and a scene
        // whose paired transforms agree on no step (a cut, or most of it moving on its own) is left as it was.
        //
        // Nor is anything moved when the step jumped, its eye moving more than 4 times as far as the frame before's step
        // did, or when the frame before's scene agreed on none: a cut too small for the cut check (RT64_SKIP_CAMERA_CUTS)
        // is then drawn as before. Excitebike 64 cuts to a camera on the ground where a rider crashed into a wall, with
        // the world's sections paired across the cut, and the nearest sections of the shot before, which the shot after
        // doesn't draw, left the bottom third of the picture the background colour in the frames between, where the
        // unpaired sections of the shot after had been drawn over it at their own places. Its camera's step otherwise grew
        // more than 4 times from one game frame to the next in 3 of 3095 on Mountain Quarry: twice as it started from
        // near rest, and once at speed.
        {
            const DrawData &curData = firstCurWorkload.drawData;
            const DrawData &prevData = firstPrevWorkload.drawData;
            thread_local std::vector<uint32_t> sceneTransforms;
            thread_local std::vector<uint32_t> pairedTransforms;
            sceneTransforms.clear();
            pairedTransforms.clear();
            for (const std::pair<uint64_t, GameCallMap> &it : curCallHashMap) {
                const GameCallMap &callMap = it.second;
                const GameIndices::Projection &projIndices = curScene.projections[callMap.sceneProjIndex];
                if (!callMap.doTransformMatching || (projIndices.workloadIndex != firstCurProjIndices.workloadIndex)) {
                    continue;
                }

                const GameCall &call = firstCurWorkload.fbPairs[projIndices.fbPairIndex].projections[projIndices.projectionIndex].gameCalls[callMap.callIndex];
                for (uint32_t m = call.callDesc.minWorldMatrix; m <= call.callDesc.maxWorldMatrix; m++) {
                    const TransformGroup &group = curData.transformGroups[curData.worldTransformGroups[m]];
                    if (group.matrixId == G_EX_ID_AUTO) {
                        sceneTransforms.emplace_back(m);
                    }
                }
            }

            std::sort(sceneTransforms.begin(), sceneTransforms.end());
            sceneTransforms.erase(std::unique(sceneTransforms.begin(), sceneTransforms.end()), sceneTransforms.end());
            // Paired transforms whose pair's index is one of this scene's workload of the frame before: another scene's pair
            // may be of another workload, and a step read from one that isn't only fails to agree with the rest.
            const size_t prevTransformCount = prevData.worldTransforms.size();
            for (uint32_t m : sceneTransforms) {
                const GameFrameMap::TransformMap &transformMap = firstCurWorkloadMap.transforms[m];
                if (transformMap.mapped && (transformMap.prevTransformIndex < prevTransformCount)) {
                    pairedTransforms.emplace_back(m);
                }
            }

            // How far a step leaves a paired transform's matrix from its pair's: the largest row's miss, against the
            // largest row, so that it reads the same at any scale.
            auto stepMiss = [&](const hlslpp::float4x4 &step, uint32_t m) {
                const hlslpp::float4x4 cur = curData.worldTransforms[m];
                const hlslpp::float4x4 prev = prevData.worldTransforms[firstCurWorkloadMap.transforms[m].prevTransformIndex];
                const hlslpp::float4x4 moved = hlslpp::mul(cur, step);
                float miss = 0.0f;
                float size = 0.0f;
                for (int r = 0; r < 4; r++) {
                    miss = std::max(miss, float(hlslpp::length(moved[r] - prev[r])));
                    size = std::max(size, float(hlslpp::length(prev[r])));
                }

                return (size > 0.0f) ? (miss / size) : FLT_MAX;
            };

            // The step of up to 16 of the paired transforms, spread over them, each tried on all of them.
            const float StepTolerance = 1e-3f;
            const uint32_t pairedCount = uint32_t(pairedTransforms.size());
            const uint32_t tries = std::min(pairedCount, 16U);
            uint32_t bestCount = 0;
            uint32_t bestIndex = UINT32_MAX;
            hlslpp::float4x4 bestStep;
            for (uint32_t t = 0; t < tries; t++) {
                const uint32_t c = pairedTransforms[(uint64_t(t) * pairedCount) / tries];
                const hlslpp::float4x4 cur = curData.worldTransforms[c];
                if (fabsf(float(hlslpp::determinant(extract3x3(cur)))) < 1e-12f) {
                    continue;
                }

                const hlslpp::float4x4 step = hlslpp::mul(hlslpp::inverse(cur), hlslpp::float4x4(prevData.worldTransforms[firstCurWorkloadMap.transforms[c].prevTransformIndex]));
                uint32_t count = 0;
                for (uint32_t m : pairedTransforms) {
                    count += (stepMiss(step, m) < StepTolerance) ? 1 : 0;
                }

                if (count > bestCount) {
                    bestCount = count;
                    bestIndex = c;
                    bestStep = step;
                }
            }

            const hlslpp::float4x4 Identity(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            bool agreed = (bestIndex != UINT32_MAX) && (bestCount >= 3) && ((bestCount * 2) > pairedCount);

            // Where the pairs agree on no step, the step that brings the most of the scene's transforms each onto one of
            // the frame before's scene's matrices, tried from each of up to 8 of them onto each of the frame before's: a
            // game that draws its world in blocks of its own making each frame, numbered as each first comes up and with
            // other vertices under each as the view moves, leaves few of them paired, but each block that stays in view
            // has its matrix of the frame before at the camera's step from its own. Excitebike 64's Desert draws its
            // ground as patches under up to 32 blocks, each a matrix of the block's place less the camera's, and its
            // pairs agreed in a third of the race's frames: the light lines across the dunes stayed in a quarter of the
            // frames that had them. The step is taken only when it brings more than half the scene's transforms, at
            // least 3, onto a matrix of the frame before's, and only in a scene of up to 32 on each side, so its cost
            // stays small.
            bool stepByPlace = false;
            if (!agreed) {
                thread_local std::vector<uint32_t> prevSceneTransforms;
                prevSceneTransforms.clear();
                for (const std::pair<uint64_t, GameCallMap> &it : prevCallHashMap) {
                    const GameCallMap &callMap = it.second;
                    const GameIndices::Projection &projIndices = prevScene.projections[callMap.sceneProjIndex];
                    if (!callMap.doTransformMatching || (projIndices.workloadIndex != firstPrevProjIndices.workloadIndex)) {
                        continue;
                    }

                    const GameCall &call = firstPrevWorkload.fbPairs[projIndices.fbPairIndex].projections[projIndices.projectionIndex].gameCalls[callMap.callIndex];
                    for (uint32_t m = call.callDesc.minWorldMatrix; m <= call.callDesc.maxWorldMatrix; m++) {
                        const TransformGroup &group = prevData.transformGroups[prevData.worldTransformGroups[m]];
                        if (group.matrixId == G_EX_ID_AUTO) {
                            prevSceneTransforms.emplace_back(m);
                        }
                    }
                }

                std::sort(prevSceneTransforms.begin(), prevSceneTransforms.end());
                prevSceneTransforms.erase(std::unique(prevSceneTransforms.begin(), prevSceneTransforms.end()), prevSceneTransforms.end());
                const uint32_t MaxPlaceTransforms = 32;
                const uint32_t sceneCount = uint32_t(sceneTransforms.size());
                if ((sceneCount >= 3) && (sceneCount <= MaxPlaceTransforms) && !prevSceneTransforms.empty() && (prevSceneTransforms.size() <= MaxPlaceTransforms)) {
                    // How far a step leaves a transform's matrix from the nearest of the frame before's, as stepMiss reads it.
                    auto placeMiss = [&](const hlslpp::float4x4 &step, uint32_t m) {
                        const hlslpp::float4x4 moved = hlslpp::mul(hlslpp::float4x4(curData.worldTransforms[m]), step);
                        float nearest = FLT_MAX;
                        for (uint32_t q : prevSceneTransforms) {
                            const hlslpp::float4x4 prev = prevData.worldTransforms[q];
                            float miss = 0.0f;
                            float size = 0.0f;
                            for (int r = 0; r < 4; r++) {
                                miss = std::max(miss, float(hlslpp::length(moved[r] - prev[r])));
                                size = std::max(size, float(hlslpp::length(prev[r])));
                            }

                            if (size > 0.0f) {
                                nearest = std::min(nearest, miss / size);
                            }
                        }

                        return nearest;
                    };

                    const uint32_t placeTries = std::min(sceneCount, 8U);
                    uint32_t placeCount = 0;
                    for (uint32_t t = 0; t < placeTries; t++) {
                        const uint32_t c = sceneTransforms[(uint64_t(t) * sceneCount) / placeTries];
                        const hlslpp::float4x4 cur = curData.worldTransforms[c];
                        if (fabsf(float(hlslpp::determinant(extract3x3(cur)))) < 1e-12f) {
                            continue;
                        }

                        const hlslpp::float4x4 inverseCur = hlslpp::inverse(cur);
                        for (uint32_t q : prevSceneTransforms) {
                            const hlslpp::float4x4 step = hlslpp::mul(inverseCur, hlslpp::float4x4(prevData.worldTransforms[q]));
                            uint32_t count = 0;
                            for (uint32_t m : sceneTransforms) {
                                count += (placeMiss(step, m) < StepTolerance) ? 1 : 0;
                            }

                            if (count > placeCount) {
                                placeCount = count;
                                bestStep = step;
                            }
                        }
                    }

                    stepByPlace = (placeCount >= 3) && ((placeCount * 2) > sceneCount);
                    agreed = stepByPlace;
                }
            }

            const float MaxJumpRatio = 4.0f;
            const float stepLength = agreed ? float(hlslpp::length(hlslpp::float3(bestStep[3].xyz))) : -1.0f;
            const float prevStepLength = (firstPrevWorkloadMap != nullptr) ? firstPrevWorkloadMap->viewProjections[firstPrevProj.transformsIndex].cameraStep : -1.0f;
            const bool jumped = (prevStepLength < 0.0f) || (stepLength > (prevStepLength * MaxJumpRatio));
            firstCurWorkloadMap.viewProjections[firstCurProj.transformsIndex].cameraStep = stepLength;
            float stepFromIdentity = 0.0f;
            for (int r = 0; r < 4; r++) {
                stepFromIdentity = std::max(stepFromIdentity, float(hlslpp::length(bestStep[r] - Identity[r])));
            }

            const bool stepMoves = stepByPlace ? (stepFromIdentity >= (StepTolerance * 0.1f)) : (agreed && (stepMiss(Identity, bestIndex) >= (StepTolerance * 0.1f)));
            {
                // TEST (not for merge): the step's jump check.
                static const bool stepLog = (getenv("RT64_TEST_STEP_LOG") != nullptr);
                if (stepLog) {
                    const uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                    uint32_t unpaired = 0;
                    for (uint32_t m : sceneTransforms) {
                        unpaired += firstCurWorkloadMap.transforms[m].mapped ? 0 : 1;
                    }

                    fprintf(stderr, "STEPLOG2 %llu len %.4f prev %.4f agreed %d jumped %d paired %u best %u unpaired %u place %d\n", (unsigned long long)ms, stepLength, prevStepLength, agreed ? 1 : 0, jumped ? 1 : 0, pairedCount, bestCount, unpaired, stepByPlace ? 1 : 0);
                }
            }
            if (agreed && !jumped && stepMoves) {
                // Drawn as the transform whose step it takes is, from its own pair of matrices; with a step found by place,
                // as a transform paired with its own matrix moved back by the step.
                for (uint32_t m : sceneTransforms) {
                    GameFrameMap::TransformMap &transformMap = firstCurWorkloadMap.transforms[m];
                    if (transformMap.mapped) {
                        continue;
                    }

                    const hlslpp::float4x4 cur = curData.worldTransforms[m];
                    const hlslpp::float4x4 moved = hlslpp::mul(cur, bestStep);
                    const TransformGroup &group = curData.transformGroups[curData.worldTransformGroups[m]];
                    if (stepByPlace) {
                        // Moving at the step already, so the step itself reads as no jump.
                        transformMap.rigidBody = RigidBody();
                        transformMap.rigidBody.linearVelocity = cur[3].xyz - moved[3].xyz;
                        transformMap.rigidBody.updateLinear(moved, cur, group.positionInterpolation);
                        transformMap.rigidBody.updateAngular(moved, cur, group.rotationInterpolation, group.scaleInterpolation, group.skewInterpolation);
                        transformMap.rigidBody.updatePerspective(moved, cur, group.perspectiveInterpolation);
                    }
                    else {
                        transformMap.rigidBody = firstCurWorkloadMap.transforms[bestIndex].rigidBody;
                    }

                    transformMap.rigidBody.linearVelocity = cur[3].xyz - moved[3].xyz;
                    transformMap.rigidBody.updateDecomposition(moved, group.decompose);
                    transformMap.rigidBody.updateDecomposition(cur, group.decompose);
                    transformMap.movedPrev = moved;
                    transformMap.moved = true;
                }
            }
        }
#endif

        if (!modifiedBuffers.empty()) {
            workloadsModified[firstCurProjIndices.workloadIndex].merge(modifiedBuffers);
        }

        // Check for tile matches.
        for (const IndexPair &indices : tileCheckSet) {
            if (firstCurWorkloadMap.tiles[indices.first].mapped) {
                continue;
            }

            if (firstCurWorkloadMap.prevTilesMapped[indices.second]) {
                continue;
            }

            // Check for tile compatibility.
            const interop::RDPTile &curTile = firstCurWorkload.drawData.rdpTiles[indices.first];
            const interop::RDPTile &prevTile = firstPrevWorkload.drawData.rdpTiles[indices.second];
            if ((curTile.fmt != prevTile.fmt) ||
                (curTile.siz != prevTile.siz) ||
                (curTile.stride != prevTile.stride) ||
                (curTile.masks != prevTile.masks) ||
                (curTile.maskt != prevTile.maskt) ||
                (curTile.shifts != prevTile.shifts) ||
                (curTile.shiftt != prevTile.shiftt) ||
                (curTile.cms != prevTile.cms) ||
                (curTile.cmt != prevTile.cmt))
            {
                continue;
            }

            GameFrameMap::TileMap &curTileMap = firstCurWorkloadMap.tiles[indices.first];
            if (firstPrevWorkloadMap != nullptr) {
                const GameFrameMap::TileMap &prevTileMap = firstPrevWorkloadMap->tiles[indices.second];
                curTileMap = prevTileMap;
            }
            
            auto modulo = [](int a, int b) {
                if (b != 0) {
                    int r = a % b;
                    return r < 0 ? r + b : r;
                }
                else {
                    return a;
                }
            };

            const float deltaUls = curTile.uls - prevTile.uls;
            const float deltaUlt = curTile.ult - prevTile.ult;
            const float deltaLrs = curTile.lrs - prevTile.lrs;
            const float deltaLrt = curTile.lrt - prevTile.lrt;
            const bool tileScrolled = (deltaUls != 0.0f) || (deltaUlt != 0.0f) || (deltaLrs != 0.0f) || (deltaLrt != 0.0f);
            const int integerUls = std::lround(curTile.uls);
            const int integerUlt = std::lround(curTile.ult);
            const int integerLrs = std::lround(curTile.lrs);
            const int integerLrt = std::lround(curTile.lrt);
            const int expectedUls = std::lround(prevTile.uls + curTileMap.deltaUls);
            const int expectedUlt = std::lround(prevTile.ult + curTileMap.deltaUlt);
            const int expectedLrs = std::lround(prevTile.lrs + curTileMap.deltaLrs);
            const int expectedLrt = std::lround(prevTile.lrt + curTileMap.deltaLrt);
            const bool wrappedUls = (curTile.cms == G_TX_WRAP) && (modulo(integerUls, curTile.masks * 4) == modulo(expectedUls, curTile.masks * 4));
            const bool wrappedUlt = (curTile.cmt == G_TX_WRAP) && (modulo(integerUlt, curTile.maskt * 4) == modulo(expectedUlt, curTile.maskt * 4));
            const bool wrappedLrs = (curTile.cms == G_TX_WRAP) && (modulo(integerLrs, curTile.masks * 4) == modulo(expectedLrs, curTile.masks * 4));
            const bool wrappedLrt = (curTile.cmt == G_TX_WRAP) && (modulo(integerLrt, curTile.maskt * 4) == modulo(expectedLrt, curTile.maskt * 4));
            const float beforeUls = curTileMap.deltaUls;
            const float beforeUlt = curTileMap.deltaUlt;
            const float beforeLrs = curTileMap.deltaLrs;
            const float beforeLrt = curTileMap.deltaLrt;
            curTileMap.deltaUls = wrappedUls || (abs(deltaUls) >= curTile.masks * 2) ? curTileMap.deltaUls : deltaUls;
            curTileMap.deltaUlt = wrappedUlt || (abs(deltaUlt) >= curTile.maskt * 2) ? curTileMap.deltaUlt : deltaUlt;
            curTileMap.deltaLrs = wrappedLrs || (abs(deltaLrs) >= curTile.masks * 2) ? curTileMap.deltaLrs : deltaLrs;
            curTileMap.deltaLrt = wrappedLrt || (abs(deltaLrt) >= curTile.maskt * 2) ? curTileMap.deltaLrt : deltaLrt;

            // A scroll is placed between frames only while it is steady, each direction by itself, from this frame's
            // own start. Drawn from where the last frame's scroll said the tile was, a texture that jumps once (a
            // picture stepping to its next cell) slid there a frame late, a slow scroll that moves every other frame
            // slid back on the frames it rested, and one that changed speed started each frame in the wrong place.
            // A direction that scrolls the same way as the last frame did, 0.4 to 2.5 times as far, is placed; the
            // last frame's amount stands in for this one's only across a wrap, where this one's can't be read. Any
            // other is drawn where the game put it. S and T are judged apart, so a drift in one doesn't stop the
            // other's flow.
            auto steady = [](float now, float before) {
                if ((before == 0.0f) || (now == 0.0f) || ((now < 0.0f) != (before < 0.0f))) {
                    return false;
                }

                const float ratio = now / before;
                return (ratio >= 0.4f) && (ratio <= 2.5f);
            };

            // A tile's two edges move together in every case seen: an edge that stands while the other flows is
            // allowed, one that does anything else is not.
            auto placed = [&](float nowA, float beforeA, float nowB, float beforeB) {
                const bool a = steady(nowA, beforeA);
                const bool b = steady(nowB, beforeB);
                const bool aStill = (nowA == 0.0f) && (beforeA == 0.0f);
                const bool bStill = (nowB == 0.0f) && (beforeB == 0.0f);
                return (a || b) && (a || aStill) && (b || bStill);
            };

            const bool placeS = placed(curTileMap.deltaUls, beforeUls, curTileMap.deltaLrs, beforeLrs);
            const bool placeT = placed(curTileMap.deltaUlt, beforeUlt, curTileMap.deltaLrt, beforeLrt);
            curTileMap.prevUls = placeS ? (curTile.uls - curTileMap.deltaUls) : curTile.uls;
            curTileMap.prevLrs = placeS ? (curTile.lrs - curTileMap.deltaLrs) : curTile.lrs;
            curTileMap.prevUlt = placeT ? (curTile.ult - curTileMap.deltaUlt) : curTile.ult;
            curTileMap.prevLrt = placeT ? (curTile.lrt - curTileMap.deltaLrt) : curTile.lrt;
            curTileMap.curUls = curTile.uls;
            curTileMap.curUlt = curTile.ult;
            curTileMap.curLrs = curTile.lrs;
            curTileMap.curLrt = curTile.lrt;

            // A slow scroll (a quarter texel every few frames, on two frames of three, or 1, 2 and 4 quarters by
            // turns) isn't steady above, so it was drawn where the game put it, a step at a time, or placed on some
            // frames and not others, which a magnified texture shows as a stutter. An edge that stepped the same way
            // at least twice in its last 16 frames and never more than a texel moves on every frame at the rate it
            // moved at over about its last 32, steered a quarter of the way each frame toward half its largest step
            // behind the game's place, which follows a scroll that speeds up: each frame starting where the last one
            // ended, never ahead of the game's own place, so a scroll that stops comes to rest where the game put
            // it, and never further behind it than two steps and a frame's move. One that steps further, turns back
            // or jumps starts over, drawn as above. A step across a masked tile's wrap is the step it took inside
            // the tile.
            const float curValues[4] = { curTile.uls, curTile.ult, curTile.lrs, curTile.lrt };
            const float rawDeltas[4] = { deltaUls, deltaUlt, deltaLrs, deltaLrt };
            const int wrapPeriods[4] = {
                (curTile.cms == G_TX_WRAP) ? curTile.masks * 4 : 0, (curTile.cmt == G_TX_WRAP) ? curTile.maskt * 4 : 0,
                (curTile.cms == G_TX_WRAP) ? curTile.masks * 4 : 0, (curTile.cmt == G_TX_WRAP) ? curTile.maskt * 4 : 0
            };

            float slowFrom[4];
            float slowTo[4];
            bool slowMoving[4];
            bool slowStanding[4];
            for (uint32_t e = 0; e < 4; e++) {
                float step = rawDeltas[e];
                const int period = wrapPeriods[e];
                if ((period > 0) && (fabsf(step) >= float(period / 2))) {
                    const int inside = modulo(std::lround(step), period);
                    step = float((inside > (period / 2)) ? (inside - period) : inside);
                }

                float &moved = curTileMap.slowMoved[e];
                float &largest = curTileMap.slowLargest[e];
                float &behind = curTileMap.slowBehind[e];
                uint16_t &frames = curTileMap.slowFrames[e];
                uint16_t &steps = curTileMap.slowSteps[e];
                const bool turned = (step != 0.0f) && (moved != 0.0f) && ((step < 0.0f) != (moved < 0.0f));
                const bool leapt = (fabsf(step) > 4.0f);
                if (turned || leapt) {
                    moved = 0.0f;
                    largest = 0.0f;
                    behind = 0.0f;
                    frames = 0;
                    steps = 0;
                }

                if (!leapt) {
                    moved += step;
                    largest = std::max(largest, fabsf(step));
                    steps = uint16_t((steps << 1) | ((step != 0.0f) ? 1 : 0));
                    frames++;
                    if (frames >= 32) {
                        frames /= 2;
                        moved /= 2.0f;
                    }
                }

                const uint32_t window = std::min(uint32_t(frames), 16U);
                const uint32_t windowMask = (1U << window) - 1;
                const uint32_t recent = steps & windowMask;
                uint32_t stepCount = 0;
                for (uint32_t r = recent; r != 0; r &= (r - 1)) {
                    stepCount++;
                }

                slowMoving[e] = (frames >= 4) && (stepCount >= 2) && (moved != 0.0f);
                slowStanding[e] = (moved == 0.0f) && (rawDeltas[e] == 0.0f);
                if (slowMoving[e]) {
                    const float rate = moved / float(frames);
                    const float direction = (rate < 0.0f) ? -1.0f : 1.0f;
                    const float band = (largest * 2.0f) + fabsf(rate);
                    const float behindStart = std::clamp(direction * (behind + step), 0.0f, band);
                    const float move = std::max(fabsf(rate) + (behindStart - (largest * 0.5f)) * 0.25f, 0.0f);
                    const float behindEnd = std::clamp(behindStart - move, 0.0f, band);
                    behind = direction * behindEnd;
                    slowFrom[e] = curValues[e] - direction * behindStart;
                    slowTo[e] = curValues[e] - direction * behindEnd;
                }
                else {
                    behind = 0.0f;
                    slowFrom[e] = curValues[e];
                    slowTo[e] = curValues[e];
                }
            }

            // Both edges of a direction scroll together, or one stands while the other scrolls, as above.
            auto slowDirection = [&](uint32_t a, uint32_t b) {
                return (slowMoving[a] || slowMoving[b]) && (slowMoving[a] || slowStanding[a]) && (slowMoving[b] || slowStanding[b]);
            };

            const bool slowS = slowDirection(0, 2);
            const bool slowT = slowDirection(1, 3);
            if (slowS) {
                curTileMap.prevUls = slowFrom[0];
                curTileMap.curUls = slowTo[0];
                curTileMap.prevLrs = slowFrom[2];
                curTileMap.curLrs = slowTo[2];
            }

            if (slowT) {
                curTileMap.prevUlt = slowFrom[1];
                curTileMap.curUlt = slowTo[1];
                curTileMap.prevLrt = slowFrom[3];
                curTileMap.curLrt = slowTo[3];
            }

            curTileMap.mapped = true;
            firstCurWorkloadMap.prevTilesMapped[indices.second] = true;
            tileInterpolationUsed = tileInterpolationUsed || tileScrolled || slowS || slowT;
        }

        // Check for look at matches.
        for (const IndexPair &indices : lookAtCheckSet) {
            if (firstCurWorkloadMap.lookAt[indices.first].mapped) {
                continue;
            }

            if (firstCurWorkloadMap.prevLookAtMapped[indices.second]) {
                continue;
            }

            GameFrameMap::LookAtMap &curLookAtMap = firstCurWorkloadMap.lookAt[indices.first];
            if (firstPrevWorkloadMap != nullptr) {
                const GameFrameMap::LookAtMap &prevLookAtMap = firstPrevWorkloadMap->lookAt[indices.second];
                curLookAtMap = prevLookAtMap;
            }

            const interop::RSPLookAt &curLookAt = firstCurWorkload.drawData.rspLookAt[indices.first];
            const interop::RSPLookAt &prevLookAt = firstPrevWorkload.drawData.rspLookAt[indices.second];
            bool lookAtMoved = true;
            curLookAtMap.mapped = true;
            curLookAtMap.deltaX = hlslpp::float3(curLookAt.x) - hlslpp::float3(prevLookAt.x);
            curLookAtMap.deltaY = hlslpp::float3(curLookAt.y) - hlslpp::float3(prevLookAt.y);
            firstCurWorkloadMap.prevLookAtMapped[indices.second] = true;
            lookAtInterpolationUsed = lookAtInterpolationUsed || lookAtMoved;
        }
    }

    void GameFrame::matchTransform(Workload &curWorkload, const Workload &prevWorkload, GameFrameMap::WorkloadMap &curWorkloadMap, const GameFrameMap::WorkloadMap *prevWorkloadMap, uint32_t curTransformIndex, uint32_t prevTransformIndex, ModifiedBuffers &modifiedBuffers) {
        GameFrameMap::TransformMap &curTransformMap = curWorkloadMap.transforms[curTransformIndex];
        if (prevWorkloadMap != nullptr) {
            curTransformMap.rigidBody = prevWorkloadMap->transforms[prevTransformIndex].rigidBody;
        }

        const hlslpp::float4x4 &curTransform = curWorkload.drawData.worldTransforms[curTransformIndex];
        const hlslpp::float4x4 &prevTransform = prevWorkload.drawData.worldTransforms[prevTransformIndex];
        const uint32_t curGroupIndex = curWorkload.drawData.worldTransformGroups[curTransformIndex];
        const TransformGroup &curGroup = curWorkload.drawData.transformGroups[curGroupIndex];
        curTransformMap.rigidBody.updateLinear(prevTransform, curTransform, curGroup.positionInterpolation);
        curTransformMap.rigidBody.updateAngular(prevTransform, curTransform, curGroup.rotationInterpolation, curGroup.scaleInterpolation, curGroup.skewInterpolation);
        curTransformMap.rigidBody.updatePerspective(prevTransform, curTransform, curGroup.perspectiveInterpolation);
        curTransformMap.rigidBody.updateDecomposition(curTransform, curGroup.decompose);
        curTransformMap.prevTransformIndex = prevTransformIndex;
        curTransformMap.mapped = true;
        curWorkloadMap.prevTransformsMapped[prevTransformIndex] = true;

#if RT64_SKIP_PLACE_SWAPS
        // Whether its vertices held still, for the pairs of the next frame (above).
        {
            const uint32_t vertexIndex = curWorkload.drawData.worldTransformVertexIndices[curTransformIndex];
            const uint32_t vertexCount = curWorkload.drawData.worldTransformVertexCount(curTransformIndex);
            const uint32_t prevIndex = prevWorkload.drawData.worldTransformVertexIndices[prevTransformIndex];
            const uint32_t prevCount = prevWorkload.drawData.worldTransformVertexCount(prevTransformIndex);
            curTransformMap.verticesStill = (vertexCount == prevCount) && ((vertexCount == 0) ||
                (memcmp(&curWorkload.drawData.posFloats[vertexIndex * 3], &prevWorkload.drawData.posFloats[prevIndex * 3], vertexCount * 3 * sizeof(float)) == 0));
        }
#endif

        uint64_t curVertexHash = 0;
        uint64_t prevVertexHash = 0;
        uint32_t curVertexIndex = curWorkload.drawData.worldTransformVertexIndices[curTransformIndex];
        uint32_t curVertexCount = curWorkload.drawData.worldTransformVertexCount(curTransformIndex);
        uint32_t prevVertexIndex = prevWorkload.drawData.worldTransformVertexIndices[prevTransformIndex];
        uint32_t prevVertexCount = prevWorkload.drawData.worldTransformVertexCount(prevTransformIndex);
        if (((curGroup.vertexInterpolation != G_EX_COMPONENT_SKIP) || (curGroup.texcoordInterpolation == G_EX_COMPONENT_AUTO)) && (curVertexCount == prevVertexCount)) {
            const std::vector<float> &curPosFloats = curWorkload.drawData.posFloats;
            const std::vector<float> &prevPosFloats = prevWorkload.drawData.posFloats;
            std::vector<float> &curVelFloats = curWorkload.drawData.velFloats;
            curVertexHash = XXH3_64bits(&curPosFloats[curVertexIndex * 3], curVertexCount * 3 * sizeof(float));
            prevVertexHash = XXH3_64bits(&prevPosFloats[prevVertexIndex * 3], prevVertexCount * 3 * sizeof(float));

            if ((curGroup.vertexInterpolation != G_EX_COMPONENT_SKIP) && (curVertexHash != prevVertexHash)) {
                const float *curPosFloatsRef = &curPosFloats[curVertexIndex * 3];
                const float *prevPosFloatsRef = &prevPosFloats[prevVertexIndex * 3];
                float *curVelFloatsRef = &curVelFloats[curVertexIndex * 3];
                for (uint32_t i = 0; i < curVertexCount; i++) {
                    for (uint32_t j = 0; j < 3; j++) {
                        curVelFloatsRef[i * 3 + j] = (curPosFloatsRef[i * 3 + j] - prevPosFloatsRef[i * 3 + j]);
                    }
                }

                modifiedBuffers.positionVelocity = true;
            }
        }

        if ((curGroup.texcoordInterpolation != G_EX_COMPONENT_SKIP) && (curVertexCount == prevVertexCount)) {
            const std::vector<float> &curTcFloats = curWorkload.drawData.tcFloats;
            const std::vector<float> &prevTcFloats = prevWorkload.drawData.tcFloats;
            std::vector<float> &curVelFloats = curWorkload.drawData.tcVelFloats;
            const hlslpp::float2 WrappingModulo = curWorkload.extended.texcoordWrapPoint;
            const hlslpp::float2 WrappingModuloHalf = WrappingModulo / 2.0f;
            uint64_t curTcHash = XXH3_64bits(&curTcFloats[curVertexIndex * 2], curVertexCount * 2 * sizeof(float));
            uint64_t prevTcHash = XXH3_64bits(&prevTcFloats[prevVertexIndex * 2], prevVertexCount * 2 * sizeof(float));
            if (((curGroup.texcoordInterpolation != G_EX_COMPONENT_AUTO) || (curVertexHash == prevVertexHash)) && (curTcHash != prevTcHash)) {
                const float *curTcFloatsRef = &curTcFloats[curVertexIndex * 2];
                const float *prevTcFloatsRef = &prevTcFloats[prevVertexIndex * 2];
                float *curVelFloatsRef = &curVelFloats[curVertexIndex * 2];
                for (uint32_t i = 0; i < curVertexCount; i++) {
                    curVelFloatsRef[i * 2 + 0] = (curTcFloatsRef[i * 2 + 0] - prevTcFloatsRef[i * 2 + 0]);
                    curVelFloatsRef[i * 2 + 1] = (curTcFloatsRef[i * 2 + 1] - prevTcFloatsRef[i * 2 + 1]);

                    if (fabsf(curVelFloatsRef[i * 2]) > WrappingModuloHalf[0]) {
                        curVelFloatsRef[i * 2] -= lround(curVelFloatsRef[i * 2] / WrappingModulo[0]) * WrappingModulo[0];
                    }

                    if (fabsf(curVelFloatsRef[i * 2 + 1]) > WrappingModuloHalf[1]) {
                        curVelFloatsRef[i * 2 + 1] -= lround(curVelFloatsRef[i * 2 + 1] / WrappingModulo[1]) * WrappingModulo[1];
                    }
                }
            }

            modifiedBuffers.texcoordVelocity = true;
        }

        // Vertices the game's CPU moves itself under a matrix it keeps (Rayman 2's menu cursor, a butterfly whose eye
        // and body it places anew every frame) moved only on the game's own frames, while what moves by its matrix
        // moved on every frame drawn. A game that says nothing of them (no matrix ID, so no vertex interpolation of its
        // choosing) has them moved between frames too, from where the frame before drew them, when the two have as
        // many vertices and none moved further than the largest side of the box they span: the same thing moving, not
        // a buffer the game filled with something else. Nor when the vertices paired in order aren't each other: a
        // buffer refilled with as many other vertices of the same model (a golf course's terrain, its chunks in another
        // order as a flyover pans) passes that test, and the frames between would draw each vertex on its way to
        // another's place, the model torn into loose triangles. So a sample of up to 64 of them is each checked against
        // every vertex of the frame before, and one that has another of those at less than a quarter of the distance to
        // its pair is paired with something else; 3 or more such, and one in 20 of the sample, leave the vertices to
        // move on the game's own frames. A mesh that moves or bends keeps each vertex nearest to its own last place.
        // In a block of its own after the extended GBI's interpolation and the texture coordinates', whose lines a game
        // fork may change in its own patches.
        const bool autoVertices = (curGroup.matrixId == G_EX_ID_AUTO) && (curGroup.vertexInterpolation == G_EX_COMPONENT_SKIP);
        if (autoVertices && (curVertexCount > 0) && (curVertexCount == prevVertexCount)) {
            const std::vector<float> &curPosFloats = curWorkload.drawData.posFloats;
            const std::vector<float> &prevPosFloats = prevWorkload.drawData.posFloats;
            std::vector<float> &curVelFloats = curWorkload.drawData.velFloats;
            const float *curPosFloatsRef = &curPosFloats[curVertexIndex * 3];
            const float *prevPosFloatsRef = &prevPosFloats[prevVertexIndex * 3];
            float minPos[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
            float maxPos[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            float maxMove = 0.0f;
            for (uint32_t i = 0; i < curVertexCount; i++) {
                for (uint32_t j = 0; j < 3; j++) {
                    const float pos = curPosFloatsRef[i * 3 + j];
                    minPos[j] = std::min(minPos[j], pos);
                    maxPos[j] = std::max(maxPos[j], pos);
                    maxMove = std::max(maxMove, fabsf(pos - prevPosFloatsRef[i * 3 + j]));
                }
            }

            const float extent = std::max(std::max(maxPos[0] - minPos[0], maxPos[1] - minPos[1]), maxPos[2] - minPos[2]);
            auto distanceSquared = [](const float *a, const float *b) {
                const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
                return (x * x) + (y * y) + (z * z);
            };

            auto pairedInOrder = [&]() {
                const uint32_t sampleStep = std::max(curVertexCount / 64, 1U);
                uint32_t sampled = 0;
                uint32_t mispaired = 0;
                for (uint32_t i = 0; i < curVertexCount; i += sampleStep) {
                    const float *curPos = &curPosFloatsRef[i * 3];
                    const float limit = distanceSquared(curPos, &prevPosFloatsRef[i * 3]) / 16.0f;
                    for (uint32_t j = 0; j < prevVertexCount; j++) {
                        if (distanceSquared(curPos, &prevPosFloatsRef[j * 3]) < limit) {
                            mispaired++;
                            break;
                        }
                    }

                    sampled++;
                }

                return (mispaired < 3) || ((mispaired * 20) < sampled);
            };

            if ((maxMove > 0.0f) && (maxMove <= extent) && pairedInOrder()) {
                float *curVelFloatsRef = &curVelFloats[curVertexIndex * 3];
                for (uint32_t i = 0; i < (curVertexCount * 3); i++) {
                    curVelFloatsRef[i] = curPosFloatsRef[i] - prevPosFloatsRef[i];
                }

                modifiedBuffers.positionVelocity = true;
            }
        }
    }
    
    void GameFrame::buildCallHashMap(uint32_t sceneProjIndex, const Workload &workload, const Projection &proj, std::vector<std::pair<uint64_t, GameCallMap>> &hashMap) const {
        for (uint32_t c = 0; c < proj.gameCallCount; c++) {
            const GameCall &call = proj.gameCalls[c];
            uint32_t matrixIdHash = 0;
            bool doTransformMatching = false;
            bool doTileInterpolation = false;
            bool doTileMatching = false;
            for (uint32_t m = call.callDesc.minWorldMatrix; m <= call.callDesc.maxWorldMatrix; m++) {
                const uint32_t groupIndex = workload.drawData.worldTransformGroups[m];
                const TransformGroup &group = workload.drawData.transformGroups[groupIndex];
                matrixIdHash = matrixIdHash * 33 ^ group.matrixId;

                const bool usesIdWithAutoOrdering = (group.matrixId != G_EX_ID_AUTO) && (group.matrixId != G_EX_ID_IGNORE) && (group.ordering == G_EX_ORDER_AUTO);
                doTransformMatching = doTransformMatching || (group.matrixId == G_EX_ID_AUTO) || usesIdWithAutoOrdering;
                doTileInterpolation = doTileInterpolation || (group.tileInterpolation != G_EX_COMPONENT_SKIP);
                doTileMatching = doTileMatching || (group.tileInterpolation == G_EX_COMPONENT_AUTO);
            }

            hashMap.emplace_back(hashFromCall(call, matrixIdHash), GameCallMap{ sceneProjIndex, c, doTransformMatching, doTileInterpolation, doTileMatching });
        }
    }

    void GameFrame::buildTransformIdMap(const Workload &workload, std::multimap<uint32_t, uint32_t> &idMap, std::vector<uint32_t> &ignoredIdVector) const {
        idMap.clear();
        ignoredIdVector.clear();

        uint32_t transformCount = uint32_t(workload.drawData.worldTransformGroups.size());
        for (uint32_t i = 0; i < transformCount; i++) {
            const uint32_t groupIndex = workload.drawData.worldTransformGroups[i];
            const TransformGroup &group = workload.drawData.transformGroups[groupIndex];
            if (group.matrixId == G_EX_ID_AUTO) {
                continue;
            }
            else if (group.matrixId == G_EX_ID_IGNORE) {
                ignoredIdVector.emplace_back(i);
            }
            else if (group.ordering == G_EX_ORDER_LINEAR) {
                idMap.emplace(group.matrixId, i);
            }
        }
    }

    uint64_t GameFrame::hashFromCall(const GameCall &call, uint32_t matrixIdHash) const {
        struct CallMatchKey {
            interop::ColorCombiner colorCombiner;
            interop::OtherMode otherMode;
            uint32_t geometryMode;
            uint32_t triangleCount;
            uint32_t matrixIdHash;
        };

        CallMatchKey key;
        key.colorCombiner = call.callDesc.colorCombiner;
        key.otherMode = call.callDesc.otherMode;
        key.geometryMode = call.callDesc.geometryMode;
        key.triangleCount = call.callDesc.triangleCount;
        key.matrixIdHash = matrixIdHash;
        return XXH3_64bits(&key, sizeof(CallMatchKey));
    }

    bool GameFrame::isDebuggerCameraEnabled(const WorkloadQueue &workloadQueue) {
        for (uint32_t w : workloads) {
            if (workloadQueue.workloads[w].debuggerCamera.enabled) {
                return true;
            }
        }

        return false;
    }

    /*
    void resetTransformMap(GameTransformMap &transformMap, const GameFrame &gameFrame) {
        transformMap.fbPairs.resize(gameFrame.fbPairCount);
        for (uint32_t f = 0; f < gameFrame.fbPairCount; f++) {
            const auto &gameFbPair = gameFrame.fbPairs[f];
            auto &mapFbPair = transformMap.fbPairs[f];
            mapFbPair.projections.resize(gameFbPair.projectionCount, { });
            for (uint32_t p = 0; p < gameFbPair.projectionCount; p++) {
                mapFbPair.projections[p].drawCalls.clear();
                mapFbPair.projections[p].drawCalls.resize(gameFbPair.projections[p].drawCallCount, { });
                mapFbPair.projections[p].transformCallMap.clear();
            }

            mapFbPair.prevFbPairIndex = 0;
            mapFbPair.mapped = false;
        }

        transformMap.transforms.clear();
        transformMap.transforms.resize(gameFrame.drawData.worldTransforms.size(), { });
    }

    void makeTransformCallMap(GameProjection &gameProj, std::multimap<uint32_t, uint32_t> &callMap) {
        for (uint32_t d = 0; d < gameProj.drawCallCount; d++) {
            const auto &curCall = gameProj.drawCalls[d];
            const uint32_t startIndex = std::max(curCall.callDesc.minWorldMatrix, static_cast<uint16_t>(1));
            for (uint32_t t = startIndex; t <= curCall.callDesc.maxWorldMatrix; t++) {
                callMap.emplace(t, d);
            }
        }
    }

    void GameFrame::matchPreviousFrame(GameFrame &prevFrame, RenderWorker *worker, BufferUploader *bufferUploader) {
        assert(worker != nullptr);
        assert(bufferUploader != nullptr);
        transformMap.submissionFrame = prevFrame.submissionFrame;
        resetTransformMap(transformMap, *this);
        transformMap.mapped = true;

        static std::vector<bool> prevTransformMapped;
        static std::vector<TransformMatchCandidate> matchCandidates;
        bool uploadVelBuffer = false;
        const auto &prevDrawData = prevFrame.drawData;
        prevTransformMapped.clear();
        prevTransformMapped.resize(prevDrawData.worldTransforms.size(), false);
        for (uint32_t f = 0; f < fbPairCount; f++) {
            if (f >= prevFrame.fbPairCount) {
                continue;
            }

            // Check if the framebuffer pairs are actually compatible.
            auto &curFbPair = fbPairs[f];
            auto &prevFbPair = prevFrame.fbPairs[f];
            if ((curFbPair.colorImage.width != prevFbPair.colorImage.width) || (curFbPair.colorImage.fmt != prevFbPair.colorImage.fmt) || (curFbPair.colorImage.siz != prevFbPair.colorImage.siz)) {
                continue;
            }

            auto &fbPairMap = transformMap.fbPairs[f];
            fbPairMap.mapped = true;
            fbPairMap.prevFbPairIndex = f;

            for (uint32_t p = 0; p < curFbPair.projectionCount; p++) {
                if (p >= prevFbPair.projectionCount) {
                    continue;
                }

                // Check if the projections are actually compatible.
                auto &curProj = curFbPair.projections[p];
                auto &prevProj = prevFbPair.projections[p];
                if (curProj.type != prevProj.type) {
                    continue;
                }

                // TODO: Support more than just perspective projections.
                if (curProj.type != GameProjection::Type::Perspective) {
                    continue;
                }

                auto &projMap = fbPairMap.projections[p];
                auto &callMap = projMap.transformCallMap;
                projMap.mapped = true;
                projMap.prevProjectionIndex = p;
                makeTransformCallMap(curProj, callMap);

                // Make a map for the previous frame if it's empty.
                auto &prevFbMap = prevFrame.transformMap.fbPairs[fbPairMap.prevFbPairIndex];
                auto &prevCallMap = prevFbMap.projections[projMap.prevProjectionIndex].transformCallMap;
                if (prevCallMap.empty()) {
                    makeTransformCallMap(prevProj, prevCallMap);
                }

                // Find all candidates for all transforms.
                // TODO: This double loop explodes in complexity. Consider building a lookup table based on the
                // compatibility of the calls to reduce the iteration time drastically. These could be combiner
                // and other mode flags.
                matchCandidates.clear();
                const hlslpp::float4x4 &curViewProj = drawData.viewProjTransforms[curProj.transformsIndex];
                const hlslpp::float4x4 &prevViewProj = drawData.viewProjTransforms[prevProj.transformsIndex];
                for (auto curIt = callMap.begin(); curIt != callMap.end();) {
                    const uint32_t curTransform = curIt->first;
                    if (!transformMap.transforms[curTransform].mapped) {
                        const auto &curMatrix = drawData.worldTransforms[curTransform];
                        for (auto prevIt = prevCallMap.begin(); prevIt != prevCallMap.end();) {
                            const uint32_t prevTransform = prevIt->first;
                            if (!prevTransformMapped[prevTransform]) {
                                const auto prevMatrix = prevDrawData.worldTransforms[prevTransform];
                                if (isCallCompatible(curProj.drawCalls[curIt->second], drawData, prevProj.drawCalls[prevIt->second], prevDrawData)) {
                                    const TransformMatchResult matchResult = computeTransformMatch(curMatrix, curViewProj, prevMatrix, prevViewProj, prevFrame.transformMap.transforms[prevTransform].rigidBody);
                                    if (matchResult.valid) {
                                        matchCandidates.push_back({ curTransform, prevTransform, matchResult.computeDifference() });
                                    }
                                }
                            }

                            do { ++prevIt; } while (prevIt != prevCallMap.end() && (prevTransform == prevIt->first));
                        }
                    }

                    do { ++curIt; } while (curIt != callMap.end() && (curTransform == curIt->first));
                }

                // Sort all the candidates and assign them.
                std::stable_sort(matchCandidates.begin(), matchCandidates.end());
                for (const TransformMatchCandidate candidate : matchCandidates) {
                    if (transformMap.transforms[candidate.curTransformIndex].mapped) {
                        continue;
                    }

                    if (prevTransformMapped[candidate.prevTransformIndex]) {
                        continue;
                    }

                    auto &curTransformMap = transformMap.transforms[candidate.curTransformIndex];
                    const hlslpp::float4x4 &prevMatrix = prevDrawData.worldTransforms[candidate.prevTransformIndex];
                    const hlslpp::float4x4 &curMatrix = drawData.worldTransforms[candidate.curTransformIndex];
                    curTransformMap.rigidBody = prevFrame.transformMap.transforms[candidate.prevTransformIndex].rigidBody;
                    curTransformMap.rigidBody.updateLinear(prevMatrix, curMatrix);
                    curTransformMap.rigidBody.updateAngular(prevMatrix, curMatrix);
                    curTransformMap.rigidBody.updateDecomposition(curMatrix);
                    curTransformMap.prevTransformIndex = candidate.prevTransformIndex;
                    curTransformMap.mapped = true;
                    prevTransformMapped[candidate.prevTransformIndex] = true;

                    const auto prevRange = prevCallMap.equal_range(candidate.prevTransformIndex);
                    const auto curRange = callMap.equal_range(candidate.curTransformIndex);
                    auto prevRangeIt = prevRange.first;
                    auto curRangeIt = curRange.first;
                    while ((prevRangeIt != prevRange.second) && (curRangeIt != curRange.second)) {
                        // Check if any material defined a custom match callback for this call.
                        GameDrawCall *curCall = &curProj.drawCalls[curRangeIt->second];
                        GameDrawCall *prevCall = &prevProj.drawCalls[prevRangeIt->second];
                        if (curCall->lerpDesc.matchCallback != nullptr) {
                            curCall->lerpDesc.matchCallback(this, curCall, &prevFrame, prevCall);
                        }

                        // Check if the mesh positions are different and compute the velocity buffer if required.
                        // TODO: Compute the velocity buffer and compute the hash some way.
                        const bool meshHashDifference = false;
                        uploadVelBuffer = uploadVelBuffer || meshHashDifference;

                        // Check if the texture hashes are different to mark it as an animated texture material.
                        bool tmemHashDifference = false;
                        if (curCall->callDesc.tileCount == prevCall->callDesc.tileCount) {
                            const uint32_t curTileBase = curCall->callDesc.tileIndex;
                            const uint32_t prevTileBase = prevCall->callDesc.tileIndex;
                            for (uint32_t t = 0; t < curCall->callDesc.tileCount && !tmemHashDifference; t++) {
                                const auto &curTile = drawData.callTiles[curTileBase + t];
                                const auto &prevTile = prevDrawData.callTiles[prevTileBase + t];
                                tmemHashDifference = (curTile.tmemHashOrID != prevTile.tmemHashOrID);
                            }
                        }

                        // TODO: Reimplement.
                        //curCall->materialDesc.lockMask = tmemHashDifference ? 1.0f : 0.0f;

                        auto &drawCall = projMap.drawCalls[curRangeIt->second];
                        drawCall.prevCallIndex = prevRangeIt->second;
                        drawCall.mapped = true;
                        prevRangeIt++;
                        curRangeIt++;
                    }
                }
            }
        }
    }
    */
};