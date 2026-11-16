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

        // RT64_EXPAND_SCREEN_VERTICES: by workload index, its modifyPosUints with each screen x a game wrote (G_MODIFYVTX's
        // XYSCREEN) scaled as its vertex's projection was for a picture wider than 4:3.
        std::vector<std::vector<uint32_t>> expandModifyPosUints;
    };
};