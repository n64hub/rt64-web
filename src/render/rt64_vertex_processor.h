//
// RT64
//

#pragma once

#include "hle/rt64_workload.h"

#include "rt64_descriptor_sets.h"
#include "rt64_rsp_processor.h"

namespace RT64 {
    struct VertexProcessor {
#   if RT64_PACKED_RSP_INPUTS
        static const uint32_t WorldInputCount = 7;
#   endif

        struct WorldCB {
            uint32_t vertexStart;
            uint32_t vertexCount;
            float prevFrameWeight;
            float curFrameWeight;
#       if RT64_PACKED_RSP_INPUTS
            uint32_t inputOffsets[WorldInputCount];
#       endif
        };

        struct ProcessParams {
            RenderWorker *worker = nullptr;
            DrawData *drawData = nullptr;
            DrawBuffers *drawBuffers = nullptr;
            OutputBuffers *outputBuffers = nullptr;
            float curFrameWeight = 1.0f;
            float prevFrameWeight = 0.0f;
#       if RT64_PACKED_RSP_INPUTS
            uint32_t packedSlot = 0;
#       endif
        };

        WorldCB worldCB;
        std::unique_ptr<RSPWorldDescriptorSet> descriptorSet;
#   if RT64_PACKED_RSP_INPUTS
        std::vector<RSPPackedInputs> packedInputs;
        uint32_t packedSlot = 0;
#   endif

        VertexProcessor(RenderDevice *device);
        ~VertexProcessor();
        void process(const ProcessParams &p);
        void recordCommandList(RenderWorker *worker, const ShaderLibrary *shaderLibrary, const OutputBuffers *outputBuffers);
    };
};
