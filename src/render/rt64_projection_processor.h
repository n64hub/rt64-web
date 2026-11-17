//
// RT64
//

#pragma once

#include "hle/rt64_game_frame.h"

#include "rt64_buffer_uploader.h"

namespace RT64 {
    struct ProjectionProcessor {
        std::unique_ptr<BufferUploader> bufferUploader;
        std::vector<BufferUploader::Upload> uploads;

        struct ProcessParams {
            RenderWorker *worker = nullptr;
            WorkloadQueue *workloadQueue = nullptr;
            GameFrame *curFrame = nullptr;
            const GameFrame *prevFrame = nullptr;
            float curFrameWeight = 1.0f;
            float prevFrameWeight = 0.0f;
            float aspectRatioScale = 1.0f;
        };

        ProjectionProcessor();
        ~ProjectionProcessor();
        void setup(RenderWorker *worker);
        void process(const ProcessParams &p);
        void processScene(const ProcessParams &p, const GameScene &scene, size_t sceneIndex);
        void upload(const ProcessParams &p);

        // RT64_EXPAND_SCREEN_VERTICES and RT64_INTERPOLATE_SCREEN_VERTICES: by workload index, its modifyPosUints with
        // each screen position a game wrote (G_MODIFYVTX's XYSCREEN) drawn between the frame before's and its own, and
        // its x scaled as its vertex's projection was for a picture wider than 4:3.
        std::vector<std::vector<uint32_t>> screenModifyPosUints;

        // RT64_INTERPOLATE_SCREEN_VERTICES: draws a workload's screen position writes between the frame before's and
        // its own in modifyPosUints, and says whether it drew any.
        static bool interpolateScreenWrites(const ProcessParams &p, uint32_t w, std::vector<uint32_t> &modifyPosUints);
    };
};