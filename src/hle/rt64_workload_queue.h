//
// RT64
//

#pragma once

#include <array>
#include <chrono>

#include "common/rt64_enhancement_configuration.h"
#include "common/rt64_profiling_timer.h"
#include "common/rt64_user_configuration.h"
#include "render/rt64_framebuffer_renderer.h"
#include "render/rt64_projection_processor.h"
#include "render/rt64_raster_shader_cache.h"
#include "render/rt64_tile_processor.h"
#include "render/rt64_look_at_processor.h"
#include "render/rt64_transform_processor.h"

#include "rt64_shared_queue_resources.h"
#include "rt64_workload.h"

#if RT_ENABLED
#   include "render/rt64_raytracing_shader_cache.h"
#endif

#if RT64_GAME_FRAME_WORKLOADS
// A game frame of several workloads is drawn once it has ended, and the one before it stays out of the writer's reach
// until then (WorkloadQueue::renderThreadLoop): the queue holds the frame before, the frame and the game's next one.
// A game can draw a frame in several, as Jet Force Gemini draws each of its frames in two (its shadows, then the
// frame) and Mario Party in up to a dozen (an RSP task for each microcode it draws with, and empty ones). A frame that
// doesn't fit ends where the queue is full and is drawn in parts, the frames between the game's showing the first part
// alone: with 16, Mario Party's board flickered without its characters, HUD and text now and then. With too few for
// the next one too the game waits for every interpolated frame to be presented: its frames take longer, its rate falls
// and the interpolated frames grow, down to a rate of one or two a second and a color target for each of hundreds of
// display frames.
#   define WORKLOAD_QUEUE_SIZE 32
#else
#   define WORKLOAD_QUEUE_SIZE 4
#endif

namespace RT64 {
    struct PresentQueue;

#if RT64_GAME_FRAME_WORKLOADS
    // How long a game frame with fewer workloads that draw than the one before is held back from ending at a wait
    // (WorkloadQueue::endedGameFrame()): two VIs at 60 Hz, which the present waiting for it is delayed by at most.
    constexpr std::chrono::milliseconds HeldFrameLimit(34);
#endif

#if defined(__EMSCRIPTEN__)
    // How many color targets a page's interpolated frames take turns in (WorkloadQueue::renderThreadLoop): 8 hold every
    // interpolated frame of a 7.5 fps game at 60 Hz, and ?uncapped's 360 Hz presents as many with any number.
    constexpr uint32_t InterpolatedTargetSlots = 8;
#endif

    struct WorkloadQueue {
        struct External {
            RenderDevice *device = nullptr;
            RenderWorker *workloadGraphicsWorker = nullptr;
            BufferUploader *workloadExtrasUploader = nullptr;
            BufferUploader *workloadVelocityUploader = nullptr;
            BufferUploader *workloadTilesUploader = nullptr;
            PresentQueue *presentQueue = nullptr;
            SharedQueueResources *sharedResources = nullptr;
            RasterShaderCache *rasterShaderCache = nullptr;
            TextureCache *textureCache = nullptr;
            const ShaderLibrary *shaderLibrary = nullptr;
            UserConfiguration::GraphicsAPI createdGraphicsAPI = UserConfiguration::GraphicsAPI::OptionCount;
#       if RT_ENABLED
            const RenderTexture *blueNoiseTexture = nullptr;
            RaytracingShaderCache *rtShaderCache = nullptr;
#       endif
        };

        struct WorkloadConfiguration {
            hlslpp::float2 resolutionScale = 1.0f;
            uint32_t downsampleMultiplier = 1;
            bool raytracingEnabled = false;
            float aspectRatioSource = 1.0f;
            float aspectRatioTarget = 1.0f;
            float aspectRatioScale = 1.0f;
            float extAspectPercentage = 1.0f;
            uint32_t targetRate = 0;
            bool postBlendNoise = false;
            bool postBlendNoiseNegative = false;
        };

        External ext;
        std::array<Workload, WORKLOAD_QUEUE_SIZE> workloads;
        int threadCursor;
        int writeCursor;
        int barrierCursor;
        std::mutex cursorMutex;
        std::condition_variable cursorCondition;
        uint64_t workloadId;
        uint64_t lastPresentId;
#   if RT64_GAME_FRAME_WORKLOADS
        // The last presents the game queued, the workload before each and the framebuffer it shows, and the newest
        // workload something waits for (waitForWorkloadId()): where a game frame ends (endedGameFrame()).
        struct QueuedPresent {
            uint64_t workloadId = 0;
            uint32_t fbAddress = 0;
        };

        std::array<QueuedPresent, 8> queuedPresents;
        uint32_t queuedPresentCursor = 0;
        uint64_t waitedWorkloadId = 0;

        // How many workloads that draw the last game frame had, and since when a frame with fewer has been held back from
        // ending where a wait came (endedGameFrame()).
        uint32_t lastFrameDrawnCount = 0;
        mutable uint64_t heldWorkloadId = 0;
        mutable std::chrono::steady_clock::time_point heldSince;
#   endif
        std::mutex workloadIdMutex;
        std::condition_variable workloadIdCondition;
        std::thread *renderThread = nullptr;
        std::thread *idleThread = nullptr;
        bool idleActive = false;
        std::mutex idleMutex;
        std::condition_variable idleCondition;
        std::mutex workerMutex;
        std::mutex threadMutex;
        std::atomic<bool> threadsRunning = false;
        std::atomic<bool> rtEnabled = false;
        std::atomic<bool> ubershadersOnly = false;
        std::atomic<bool> ubershadersVisible = false;
        std::unique_ptr<FramebufferRenderer> framebufferRenderer;
        std::unique_ptr<RenderFramebufferManager> renderFramebufferManager;
        TileProcessor tileProcessor;
        LookAtProcessor lookAtProcessor;
        TransformProcessor transformProcessor;
        ProjectionProcessor projectionProcessor;
        std::unique_ptr<RSPProcessor> rspProcessor;
        std::unique_ptr<VertexProcessor> vertexProcessor;
        std::unique_ptr<RenderTarget> dummyDepthTarget;
#   if RT64_WGSL
        DummyDepthTargets dummyDepthTargets;
#   endif
#   if defined(__EMSCRIPTEN__)
        // present-stats: the game frames since statsStart, and the frames drawn for them (renderThreadLoop()).
        uint32_t statsGameFrames = 0;
        uint32_t statsFramesDrawn = 0;
        Timestamp statsStart;
#   endif
        std::unique_ptr<RenderQueryPool> queryPool;
        FramebufferChangePool scratchFbChangePool;
        ProfilingTimer rendererCPUProfiler = ProfilingTimer(120);
        ProfilingTimer rendererGPUProfiler = ProfilingTimer(120);
        ProfilingTimer matchingProfiler = ProfilingTimer(120);
        ProfilingTimer workloadProfiler = ProfilingTimer(120);
        std::array<GameFrame, 2> gameFrames;
        uint32_t prevFrameIndex = uint32_t(gameFrames.size()) - 1;
        uint32_t curFrameIndex = 0;

        WorkloadQueue();
        ~WorkloadQueue();
        void reset();
        void advanceToNextWorkload();
#   if RT64_GAME_FRAME_WORKLOADS
        void presentQueued(uint64_t presentWorkloadId, uint32_t fbAddress);
        uint32_t endedGameFrame(int startCursor) const;
#   endif
        void repeatLastWorkload();
        uint32_t previousWriteCursor() const;
        void waitForIdle();
        void waitForWorkloadId(uint64_t waitId);
        void setup(const External &ext);
        void updateMultisampling();
        void threadConfigurationUpdate(hlslpp::uint2 viFbSize, WorkloadConfiguration &workloadConfig);
        void threadConfigurationValidate();
        void threadRenderFrame(GameFrame &curFrame, const GameFrame &prevFrame, const WorkloadConfiguration &workloadConfig,
            const DebuggerRenderer &debuggerRenderer, const DebuggerCamera &debuggerCamera, float curFrameWeight, float prevFrameWeight,
            float deltaTimeMs, RenderTargetKey overrideTargetKey, uint32_t overrideTargetWorkload, int32_t overrideTargetFbPairIndex, RenderTarget *overrideTarget,
            uint32_t overrideTargetModifier, bool uploadVelocity, bool uploadExtras, bool interpolateTiles, bool interpolateLookAts);

        void threadAdvanceBarrier();
        void threadAdvanceWorkloadId(uint64_t newWorkloadId);
        void renderThreadLoop();
        void idleThreadLoop();
        bool isWorkloadDone(uint64_t id);
    };
};