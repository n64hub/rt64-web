//
// RT64
//

#pragma once

#include "hle/rt64_workload.h"

#include "rt64_descriptor_sets.h"

namespace RT64 {
#if RT64_PACKED_RSP_INPUTS
    // WebGPU guarantees only 8 storage buffers per shader stage, and RSPProcessCS reads 17 inputs and RSPWorldCS 7. So
    // those shaders, built with PACKED_RSP_INPUTS, read all their inputs from one buffer: right before the dispatch,
    // each input's buffer is copied into it whole, one after the other, and the shader gets where each one starts in
    // its push constants. The inputs keep their own buffers, uploads and other readers, so nothing else changes, for
    // a few hundred KB of copies a frame. The RT64_PACKED_RSP_INPUTS build does this on every device, to test it.
    // A processor keeps one of these for each workload of a frame (its slot), and copies only the inputs uploaded
    // since that slot's last copy (BufferPair's uploadSerial): an interpolated frame draws every workload again, most
    // inputs the same as the last time, and copying all of them each time was a third of a page's WebGPU commands in a
    // frame of 16 workloads, every one of them work for the browser's GPU process.
    struct RSPPackedInputs {
        struct Copy {
            const BufferPair *source;
            uint64_t offset;
            uint64_t size;
        };

        // What the buffer holds at an input's place, from the last copy into it.
        struct Copied {
            const RenderBuffer *buffer = nullptr;
            uint64_t uploadSerial = 0;
            uint64_t offset = 0;
            uint64_t size = 0;

            bool operator==(const Copied &other) const {
                return (buffer == other.buffer) && (uploadSerial == other.uploadSerial) && (offset == other.offset) && (size == other.size);
            }
        };

        std::unique_ptr<RenderBuffer> buffer;
        uint64_t allocatedSize = 0;
        std::vector<Copy> copies;
        std::vector<Copied> copied;

        // Lays out the inputs, writes where each one starts into offsets, and makes the buffer big enough to hold them.
        // An input's allocated size is a multiple of 256 (BufferUploader), so every offset is aligned for any load.
        void update(RenderDevice *device, const BufferPair *const *inputs, uint32_t *offsets, uint32_t inputCount) {
            copies.clear();
            uint64_t size = 0;
            for (uint32_t i = 0; i < inputCount; i++) {
                offsets[i] = uint32_t(size);
                if (inputs[i]->get() != nullptr) {
                    copies.push_back({ inputs[i], size, inputs[i]->allocatedSize });
                    size += inputs[i]->allocatedSize;
                }
            }

            if (allocatedSize < size) {
                allocatedSize = (((size * 3) / 2) + 255) & ~uint64_t(255);
                buffer = device->createBuffer(RenderBufferDesc::DefaultBuffer(allocatedSize, RenderBufferFlag::STORAGE));
                copied.clear();
            }
        }

        // After the uploads it reads are recorded, which counts them.
        void recordCopies(RenderCommandList *commandList) {
            copied.resize(copies.size());
            bool copying = false;
            for (size_t i = 0; i < copies.size(); i++) {
                const Copy &copy = copies[i];
                const Copied now = { copy.source->get(), copy.source->uploadSerial, copy.offset, copy.size };
                if (copied[i] == now) {
                    continue;
                }

                if (!copying) {
                    commandList->barriers(RenderBarrierStage::COPY, RenderBufferBarrier(buffer.get(), RenderBufferAccess::WRITE));
                    copying = true;
                }

                commandList->copyBufferRegion(buffer->at(copy.offset), copy.source->get()->at(0), copy.size);
                copied[i] = now;
            }

            if (copying) {
                commandList->barriers(RenderBarrierStage::COMPUTE, RenderBufferBarrier(buffer.get(), RenderBufferAccess::READ));
            }
        }
    };
#endif

    struct RSPProcessor {
#   if RT64_PACKED_RSP_INPUTS
        static const uint32_t ProcessInputCount = 17;
#   endif

        struct ProcessCB {
            uint32_t vertexStart;
            uint32_t vertexCount;
            float prevFrameWeight;
            float curFrameWeight;
#       if RT64_PACKED_RSP_INPUTS
            uint32_t inputOffsets[ProcessInputCount];
#       endif
        };

        struct ModifyCB {
            uint32_t modifyCount;
        };

        struct ProcessParams {
            RenderWorker *worker = nullptr;
            DrawData *drawData = nullptr;
            DrawBuffers *drawBuffers = nullptr;
            OutputBuffers *outputBuffers = nullptr;
            float prevFrameWeight = 0.0f;
            float curFrameWeight = 1.0f;
#       if RT64_PACKED_RSP_INPUTS
            uint32_t packedSlot = 0;
#       endif
        };

        ProcessCB processCB;
        ModifyCB modifyCB;
        std::unique_ptr<RSPProcessDescriptorSet> processSet;
        std::unique_ptr<RSPModifyDescriptorSet> modifySet;
#   if RT64_PACKED_RSP_INPUTS
        std::vector<RSPPackedInputs> packedInputs;
        uint32_t packedSlot = 0;
#   endif

        RSPProcessor(RenderDevice *device);
        ~RSPProcessor();
        void process(const ProcessParams &p);
        void recordCommandList(RenderWorker *worker, const ShaderLibrary *shaderLibrary, const OutputBuffers *outputBuffers);
    };
};
