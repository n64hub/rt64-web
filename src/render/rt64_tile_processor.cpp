//
// RT64
//

#include "rt64_tile_processor.h"

#include "hle/rt64_game_frame.h"
#include "hle/rt64_workload_queue.h"

namespace RT64 {
    // TileProcessor

    TileProcessor::TileProcessor() { }

    TileProcessor::~TileProcessor() { }

    void TileProcessor::setup(RenderWorker *worker) {
        bufferUploader = std::make_unique<BufferUploader>(worker->device);
    }

    void TileProcessor::process(const ProcessParams &p) {
        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            DrawData &drawData = workload.drawData;
            const bool prevFrameValid = (p.prevFrame != nullptr) && p.curFrame->frameMap.workloads[w].mapped;
            const auto &curRdpTiles = drawData.rdpTiles;
            if (prevFrameValid) {
                const GameFrameMap::WorkloadMap &workloadMap = p.curFrame->frameMap.workloads[w];
                auto &lerpRdpTiles = drawData.lerpRdpTiles;
                lerpRdpTiles = curRdpTiles;

                for (size_t t = 0; t < curRdpTiles.size(); t++) {
                    const GameFrameMap::TileMap &tileMap = workloadMap.tiles[t];
                    if (!tileMap.mapped) {
                        continue;
                    }

                    interop::RDPTile &lerpTile = lerpRdpTiles[t];
                    lerpTile.uls = tileMap.prevUls + (tileMap.curUls - tileMap.prevUls) * p.curFrameWeight;
                    lerpTile.ult = tileMap.prevUlt + (tileMap.curUlt - tileMap.prevUlt) * p.curFrameWeight;
                    lerpTile.lrs = tileMap.prevLrs + (tileMap.curLrs - tileMap.prevLrs) * p.curFrameWeight;
                    lerpTile.lrt = tileMap.prevLrt + (tileMap.curLrt - tileMap.prevLrt) * p.curFrameWeight;
                }
            }
        }
    }

    void TileProcessor::upload(const ProcessParams &p) {
        uploads.clear();

        for (uint32_t w : p.curFrame->workloads) {
            const bool prevFrameValid = (p.prevFrame != nullptr) && p.curFrame->frameMap.workloads[w].mapped;
            if (prevFrameValid) {
                Workload &workload = p.workloadQueue->workloads[w];
                DrawBuffers &drawBuffers = workload.drawBuffers;
                const DrawData &drawData = workload.drawData;
                std::pair<size_t, size_t> uploadRange = { 0, drawData.rdpTiles.size() };
                assert(drawData.rdpTiles.size() == drawData.lerpRdpTiles.size());
                uploads.emplace_back(BufferUploader::Upload{ drawData.lerpRdpTiles.data(), uploadRange, sizeof(interop::RDPTile), RenderBufferFlag::STORAGE, {}, &drawBuffers.rdpTilesBuffer });
            }
        }

        if (!uploads.empty()) {
            bufferUploader->submit(p.worker, uploads);
        }
    }
};