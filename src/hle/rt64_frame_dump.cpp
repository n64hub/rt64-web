//
// RT64
//

#include "rt64_frame_dump.h"

#include <cstdarg>
#include <unordered_map>

#include "../include/rt64_extended_gbi.h"
#include "shared/rt64_f3d_defines.h"

#include "rt64_game_frame.h"
#include "rt64_workload_queue.h"

#if !defined(__EMSCRIPTEN__)
#   include <zstd.h>
#endif

namespace RT64 {
    // RT64_FRAME_DUMP's compression (rt64_frame_dump.h, FrameDump::writeCompressed()): here, where zstd's header is,
    // and a browser's build, which writes its frames raw, links no compressor.
    bool frameDumpCompress(void *&context, const std::vector<uint8_t> &src, std::vector<uint8_t> &dst, int level, const char *&error) {
#   if defined(__EMSCRIPTEN__)
        error = "a browser's build doesn't compress";
        return false;
#   else
        if (context == nullptr) {
            context = ZSTD_createCCtx();
        }

        dst.resize(ZSTD_compressBound(src.size()));
        const size_t bytes = ZSTD_compressCCtx(static_cast<ZSTD_CCtx *>(context), dst.data(), dst.size(), src.data(), src.size(), level);
        if (ZSTD_isError(bytes)) {
            error = ZSTD_getErrorName(bytes);
            return false;
        }

        dst.resize(bytes);
        return true;
#   endif
    }

    // draws.bin's lines (rt64_frame_dump.h): appended to the game frame's text.
    static void appendLine(std::string &text, const char *format, ...) {
        char line[512];
        va_list args;
        va_start(args, format);
        const int length = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        if (length > 0) {
            text.append(line, std::min(size_t(length), sizeof(line) - 1));
        }
    }

    static char callType(const Projection &proj, const GameCall &call) {
        if (call.callDesc.otherMode.cycleType() == G_CYC_FILL) {
            return 'F';
        }

        switch (proj.type) {
        case Projection::Type::Perspective:
            return 'P';
        case Projection::Type::Orthographic:
            return 'O';
        case Projection::Type::Rectangle:
            return 'R';
        case Projection::Type::Triangle:
            return 'T';
        default:
            return 'N';
        }
    }

    // Set on the workload queue's thread, the one that draws the frames presented: State draws a framebuffer for
    // render-to-RAM on the game's thread with the same renderer, and those draws are neither shown nor this thread's.
    static thread_local bool drawsThread = false;

    void FrameDump::addGameFrame(const WorkloadQueue &workloadQueue, const GameFrame &curFrame) {
        drawsThread = true;
        if (drawsOn && !drawsText.empty()) {
            std::unique_lock<std::mutex> lock(mutex);
            if (drawsQueue.size() < MaxDrawsQueued) {
                drawsQueue.emplace_back(drawsWorkloadId, std::move(drawsText));
                condition.notify_one();
            }
        }

        drawsText.clear();
        drawsRendered = 0;
        drawsFbWorkload = nullptr;
        drawsOn = inWindow() && !curFrame.workloads.empty();
        if (!drawsOn) {
            return;
        }

        const Workload &lastWorkload = workloadQueue.workloads[curFrame.workloads.back()];
        drawsWorkloadId = lastWorkload.workloadId;
        appendLine(drawsText, "F\t%llu\t%d\n", (unsigned long long)(drawsWorkloadId), curFrame.matched ? 1 : 0);

        // A scene's view-projections are paired in its first workload's map (GameFrame::matchScene()).
        std::unordered_map<uint64_t, uint32_t> sceneFirstWorkload;
        auto addScenes = [&](const std::vector<GameScene> &scenes) {
            for (const GameScene &scene : scenes) {
                if (scene.projections.empty()) {
                    continue;
                }

                for (const GameIndices::Projection &indices : scene.projections) {
                    const uint64_t key = (uint64_t(indices.workloadIndex) << 40) | (uint64_t(indices.fbPairIndex) << 20) | indices.projectionIndex;
                    sceneFirstWorkload[key] = scene.projections[0].workloadIndex;
                }
            }
        };

        addScenes(curFrame.perspectiveScenes);
        addScenes(curFrame.orthographicScenes);

        const std::vector<GameFrameMap::WorkloadMap> &maps = curFrame.frameMap.workloads;
        for (uint32_t workloadIndex : curFrame.workloads) {
            const Workload &workload = workloadQueue.workloads[workloadIndex];
            const GameFrameMap::WorkloadMap *map = (curFrame.matched && (workloadIndex < maps.size()) && maps[workloadIndex].mapped) ? &maps[workloadIndex] : nullptr;
            const unsigned long long workloadId = (unsigned long long)(workload.workloadId);
            if (map != nullptr) {
                appendLine(drawsText, "W\t%llu\t%llu\t%d\t%u\n", workloadId, (unsigned long long)(workloadQueue.workloads[map->prevWorkloadIndex].workloadId),
                    map->cameraCut ? 1 : 0, workload.fbPairCount);
            }
            else {
                appendLine(drawsText, "W\t%llu\t-\t0\t%u\n", workloadId, workload.fbPairCount);
            }

            const DrawData &drawData = workload.drawData;
            for (uint32_t f = 0; f < workload.fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                for (uint32_t p = 0; p < fbPair.projectionCount; p++) {
                    const Projection &proj = fbPair.projections[p];
                    const bool usesViewport = proj.usesViewport();
                    char viewProjPrev[16] = "-";
                    if (usesViewport && curFrame.matched) {
                        const uint64_t key = (uint64_t(workloadIndex) << 40) | (uint64_t(f) << 20) | p;
                        auto it = sceneFirstWorkload.find(key);
                        const uint32_t firstIndex = (it != sceneFirstWorkload.end()) ? it->second : workloadIndex;
                        if (firstIndex < maps.size()) {
                            const std::vector<GameFrameMap::ViewProjectionMap> &viewProjs = maps[firstIndex].viewProjections;
                            if ((proj.transformsIndex < viewProjs.size()) && viewProjs[proj.transformsIndex].mapped) {
                                snprintf(viewProjPrev, sizeof(viewProjPrev), "%u", viewProjs[proj.transformsIndex].prevTransformIndex);
                            }
                        }
                    }

                    for (uint32_t d = 0; d < proj.gameCallCount; d++) {
                        const GameCall &call = proj.gameCalls[d];
                        const DrawCall &desc = call.callDesc;
                        const char type = callType(proj, call);
                        char world[64] = "-\t-\t-\t-";
                        if (usesViewport) {
                            char worldPrev[16] = "-";
                            char worldFlags[4] = "-";
                            if ((map != nullptr) && (desc.minWorldMatrix < map->transforms.size())) {
                                const GameFrameMap::TransformMap &transformMap = map->transforms[desc.minWorldMatrix];
                                if (transformMap.mapped) {
                                    snprintf(worldPrev, sizeof(worldPrev), "%u", transformMap.prevTransformIndex);
                                }

                                int flagCount = 0;
                                if (transformMap.moved) {
                                    worldFlags[flagCount++] = 'm';
                                }

                                if (transformMap.verticesStill) {
                                    worldFlags[flagCount++] = 's';
                                }

                                if (flagCount > 0) {
                                    worldFlags[flagCount] = '\0';
                                }
                            }

                            snprintf(world, sizeof(world), "%u\t%u\t%s\t%s", desc.minWorldMatrix, desc.maxWorldMatrix, worldPrev, worldFlags);
                        }

                        char tiles[64] = "-\t0\t-\t0";
                        if ((desc.tileCount > 0) && (desc.tileIndex < drawData.callTiles.size())) {
                            bool copy = false;
                            bool synced = false;
                            for (uint32_t t = 0; (t < desc.tileCount) && ((desc.tileIndex + t) < drawData.callTiles.size()); t++) {
                                copy = copy || drawData.callTiles[desc.tileIndex + t].tileCopyUsed;
                                synced = synced || drawData.callTiles[desc.tileIndex + t].syncRequired;
                            }

                            const bool tilePaired = (map != nullptr) && (desc.tileIndex < map->tiles.size()) && map->tiles[desc.tileIndex].mapped;
                            snprintf(tiles, sizeof(tiles), "%016llx\t%u\t%s\t%d", (unsigned long long)(drawData.callTiles[desc.tileIndex].tmemHashOrID), desc.tileCount,
                                copy ? (synced ? "cs" : "c") : (synced ? "s" : "-"), tilePaired ? 1 : 0);
                        }

                        char rectMove[80] = "-";
                        if ((map != nullptr) && (desc.callIndex < map->rectMoves.size())) {
                            const hlslpp::float4 &move = map->rectMoves[desc.callIndex];
                            if ((float(move.x) != 0.0f) || (float(move.y) != 0.0f) || (float(move.z) != 0.0f) || (float(move.w) != 0.0f)) {
                                snprintf(rectMove, sizeof(rectMove), "%.2f,%.2f,%.2f,%.2f", float(move.x), float(move.y), float(move.z), float(move.w));
                            }
                        }

                        char rect[80] = "-";
                        if ((type == 'R') || (type == 'F')) {
                            snprintf(rect, sizeof(rect), "%.2f,%.2f,%.2f,%.2f", desc.rect.ulx / 4.0f, desc.rect.uly / 4.0f, desc.rect.lrx / 4.0f, desc.rect.lry / 4.0f);
                        }

                        appendLine(drawsText, "C\t%llu\t%u\t%u\t%c\t%u\t%u\t%s\t%s\t%s\t%s\t%s\t%s\n", workloadId, f, p, type, desc.callIndex, desc.triangleCount,
                            usesViewport ? std::to_string(proj.transformsIndex).c_str() : "-", viewProjPrev, world, tiles, rectMove, rect);
                    }
                }
            }
        }
    }

    void FrameDump::addRenderedFrame(float weight) {
        if (!drawsOn) {
            return;
        }

        appendLine(drawsText, "R\t%u\t%.4f\n", drawsRendered++, weight);
        drawsFbWorkload = nullptr;
    }

    void FrameDump::addDraw(const Workload &workload, uint32_t fbPairIndex, const Projection &proj, const GameCall &call, char type,
        int32_t left, int32_t top, int32_t right, int32_t bottom, float scaleX, float scaleY, float offsetX, float offsetY,
        float targetWidth, float targetHeight, float resolutionScaleX, float resolutionScaleY, uint32_t colorWidth, uint32_t colorHeight,
        float aspectRatioScale)
    {
        if (!drawsThread || !drawsOn || (drawsRendered == 0)) {
            return;
        }

        const unsigned long long workloadId = (unsigned long long)(workload.workloadId);
        if ((drawsFbWorkload != &workload) || (drawsFbPairIndex != fbPairIndex)) {
            drawsFbWorkload = &workload;
            drawsFbPairIndex = fbPairIndex;
            appendLine(drawsText, "B\t%llu\t%u\t%08x\t%.0f\t%.0f\t%.0f\t%.0f\n", workloadId, fbPairIndex,
                workload.fbPairs[fbPairIndex].colorImage.address, targetWidth, targetHeight, float(colorWidth) * resolutionScaleX,
                float(colorHeight) * resolutionScaleY);
        }

        // Where in the target, before the scissor: a fill rect's is its rect, a rect's its quad from -1 to 1 placed by the
        // screen scale and offset (RasterVS.hlsl skips its vertices' transform), and triangles the box of what of them lies past
        // the near plane.
        const DrawCall &desc = call.callDesc;
        const DrawData &drawData = workload.drawData;
        // posScreen is in the game's pixels across the target's whole size (FramebufferRenderer's fbParams.resolution).
        const float fbWidth = (resolutionScaleX > 0.0f) ? (targetWidth / resolutionScaleX) : 0.0f;
        const float fbHeight = (resolutionScaleY > 0.0f) ? (targetHeight / resolutionScaleY) : 0.0f;
        float x0 = float(left), y0 = float(top), x1 = float(right), y1 = float(bottom);
        auto toTarget = [&](float ndcX, float ndcY, float &x, float &y) {
            x = ((ndcX * scaleX + offsetX) + 1.0f) * targetWidth * 0.5f;
            y = (1.0f - (ndcY * scaleY + offsetY)) * targetHeight * 0.5f;
        };

        if (type == 'R') {
            toTarget(-1.0f, 1.0f, x0, y0);
            toTarget(1.0f, -1.0f, x1, y1);
        }
        else if (((type == 'P') || (type == 'O')) && (desc.triangleCount > 0) && (fbWidth > 0.0f) && (fbHeight > 0.0f)) {
            // The projection's x is narrowed about its viewport's centre where ProjectionProcessor widens the picture
            // (a viewport across the scissor's width at G_EX_ASPECT_AUTO, or G_EX_ASPECT_ADJUST): posScreen is the game's.
            float narrowing = 1.0f;
            const interop::RSPViewport &viewport = drawData.rspViewports[proj.transformsIndex];
            if (aspectRatioScale != 1.0f) {
                const TransformGroup &group = drawData.transformGroups[drawData.viewProjTransformGroups[proj.transformsIndex]];
                bool adjust = (group.aspectMode == G_EX_ASPECT_ADJUST);
                if (group.aspectMode == G_EX_ASPECT_AUTO) {
                    const FramebufferPair &fbPair = workload.fbPairs[fbPairIndex];
                    const int16_t *viewportClipRatios = &drawData.viewportClipRatios[proj.transformsIndex * 4];
                    const FixedRect intersectionRect = proj.scissorRect.intersection(viewport.rect(viewportClipRatios));
                    adjust = !intersectionRect.isEmpty() && (drawData.viewportOrigins[proj.transformsIndex] == G_EX_ORIGIN_NONE) &&
                        (intersectionRect.ulx <= fbPair.scissorRect.ulx) && (intersectionRect.lrx >= fbPair.scissorRect.lrx) &&
                        (intersectionRect.width(true, true) > intersectionRect.height(true, true));
                }

                narrowing = adjust ? (1.0f / aspectRatioScale) : 1.0f;
            }

            // Each triangle is cut where it crosses the near plane, z = -w in the game's clip space, so a triangle reaching
            // past it boxes only what is drawn of it: a vertex behind the camera projects through the centre, and one
            // just in front of it, which the GPU clips, far past the view. A cut's point is projected by the projection's
            // viewport as the RSP projects posScreen.
            const float centreX = float(viewport.translate.x);
            auto nearDistance = [](const hlslpp::float4 &c) {
                return float(c[2]) + float(c[3]);
            };

            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            auto addScreen = [&](float screenX, float screenY) {
                const float sx = centreX + (screenX - centreX) * narrowing;
                float x, y;
                toTarget(sx / (fbWidth * 0.5f) - 1.0f, 1.0f - screenY / (fbHeight * 0.5f), x, y);
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            };

            auto addClip = [&](const hlslpp::float4 &c) {
                addScreen(float(c[0]) / float(c[3]) * float(viewport.scale.x) + float(viewport.translate.x),
                    -float(c[1]) / float(c[3]) * float(viewport.scale.y) + float(viewport.translate.y));
            };

            const size_t vertexCount = std::min(drawData.posScreen.size(), drawData.posTransformed.size());
            for (uint32_t i = 0; (i + 2) < desc.triangleCount * 3; i += 3) {
                const size_t faceIndex = size_t(call.meshDesc.faceIndicesStart) + i;
                if ((faceIndex + 2) >= drawData.faceIndices.size()) {
                    break;
                }

                uint32_t v[3];
                bool valid = true;
                bool allFront = true;
                for (uint32_t j = 0; j < 3; j++) {
                    v[j] = drawData.faceIndices[faceIndex + j];
                    valid = valid && (v[j] < vertexCount);
                    allFront = allFront && valid && (nearDistance(drawData.posTransformed[v[j]]) > 0.0f);
                }

                if (!valid) {
                    continue;
                }

                if (allFront) {
                    for (uint32_t j = 0; j < 3; j++) {
                        addScreen(float(drawData.posScreen[v[j]][0]), float(drawData.posScreen[v[j]][1]));
                    }

                    continue;
                }

                for (uint32_t j = 0; j < 3; j++) {
                    const hlslpp::float4 &p = drawData.posTransformed[v[j]];
                    const hlslpp::float4 &q = drawData.posTransformed[v[(j + 1) % 3]];
                    const float pd = nearDistance(p), qd = nearDistance(q);
                    if ((pd > 0.0f) && (float(p[3]) > 0.0f)) {
                        addClip(p);
                    }

                    if ((pd > 0.0f) != (qd > 0.0f)) {
                        const hlslpp::float4 cut = p + (q - p) * (pd / (pd - qd));
                        if (float(cut[3]) > 0.0f) {
                            addClip(cut);
                        }
                    }
                }
            }

            if (minX <= maxX) {
                x0 = minX;
                y0 = minY;
                x1 = maxX;
                y1 = maxY;
            }
        }

        const int32_t l = std::max(left, int32_t(std::floor(x0)));
        const int32_t t = std::max(top, int32_t(std::floor(y0)));
        const int32_t r = std::min(right, int32_t(std::ceil(x1)));
        const int32_t b = std::min(bottom, int32_t(std::ceil(y1)));
        if ((l >= r) || (t >= b)) {
            return;
        }

        appendLine(drawsText, "D\t%llu\t%u\t%d\t%d\t%d\t%d\n", workloadId, desc.callIndex, l, t, r, b);
    }
};
