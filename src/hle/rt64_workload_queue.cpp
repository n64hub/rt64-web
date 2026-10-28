//
// RT64
//

#include "rt64_workload_queue.h"

#include "common/rt64_thread.h"

#include "rt64_present_queue.h"

#include <cstdio>
#include <cstdlib>

#if defined(__EMSCRIPTEN__)
#   include <emscripten.h>
#endif

#define ENABLE_HIGH_RESOLUTION_RENDERER 1

namespace RT64 {
    // WorkloadQueue

    WorkloadQueue::WorkloadQueue() {
        reset();
    }

    WorkloadQueue::~WorkloadQueue() {
        threadsRunning = false;
        cursorCondition.notify_all();
        idleCondition.notify_all();

        if (renderThread != nullptr) {
            renderThread->join();
            delete renderThread;
        }

        if (idleThread != nullptr) {
            idleThread->join();
            delete idleThread;
        }

        workloadIdCondition.notify_all();
    }

    void WorkloadQueue::reset() {
        for (Workload &w : workloads) {
            w.reset();
        }

        threadCursor = 0;
        writeCursor = 0;
        barrierCursor = int(workloads.size()) - 1;
        workloadId = 0;
        lastPresentId = 0;
#   if RT64_GAME_FRAME_WORKLOADS
        queuedPresents = {};
        queuedPresentCursor = 0;
        waitedWorkloadId = 0;
        lastFrameDrawnCount = 0;
        lastFrameShownFbAddress = 0;
        frameBeforeShownFbAddress = 0;
        lastFrameCut = false;
        heldWorkloadId = 0;
#   endif
    }

    void WorkloadQueue::advanceToNextWorkload() {
        int nextWriteCursor = (writeCursor + 1) % workloads.size();

        // Stall the thread until the barrier is lifted if we're trying to write on a workload being used by the GPU.
        bool waitForBarrier;
        do {
            const std::scoped_lock lock(cursorMutex);
            waitForBarrier = (nextWriteCursor == barrierCursor);
        } while (waitForBarrier);

        // Modify the cursor and notify anything waiting on the queue.
        {
            const std::scoped_lock lock(cursorMutex);
            writeCursor = nextWriteCursor;
        }

        cursorCondition.notify_all();
    }

#if RT64_GAME_FRAME_WORKLOADS
    // A present the game queued after the workload presentWorkloadId (State::updateScreen()), showing fbAddress.
    void WorkloadQueue::presentQueued(uint64_t presentWorkloadId, uint32_t fbAddress) {
        {
            const std::scoped_lock lock(cursorMutex);
            queuedPresents[queuedPresentCursor] = { presentWorkloadId, fbAddress };
            queuedPresentCursor = (queuedPresentCursor + 1) % queuedPresents.size();
            if (frameLog) {
                fprintf(stderr, "PQ t=%lld wl=%llu fb=%08X\n", logTime(), (unsigned long long)(presentWorkloadId), fbAddress);
            }
        }

        cursorCondition.notify_all();
    }

    // Whether the game waits in PresentQueue::advanceToNextPresent() for the present thread to move on, so it can queue
    // no workload until a frame ends: it is there (presentAdvancing) and the present queue is full, as it checks there.
    // A full present queue alone isn't that: the game goes on queueing workloads until its next present, and the rest of
    // a held frame most often comes before it. Called with cursorMutex held, so the present queue's is taken under this
    // queue's: the present thread takes none of this queue's under its own but workloadIdMutex, which is held alone
    // everywhere.
    bool WorkloadQueue::presentWaiting() const {
        if (!presentAdvancing) {
            return false;
        }

        PresentQueue &presentQueue = *ext.presentQueue;
        const std::scoped_lock lock(presentQueue.cursorMutex);
        return ((presentQueue.writeCursor + 1) % int(presentQueue.presents.size())) == presentQueue.barrierCursor;
    }

    // How many of the queued workloads from startCursor on are a game frame that has ended, or 0 while it may go on. A
    // game can draw a frame in several workloads, one for each RSP task it ends with a full sync: one for each camera,
    // and empty ones besides, or its shadows' render targets before the frame, as Jet Force Gemini does. A frame is
    // what the workloads up to its present draw, so it ends:
    // - at a present after one of its workloads that shows the framebuffer it drew to (Workload::shownFbAddress), once
    //   it has as many workloads that draw as the frame before had, or else held as at a wait below: the VI can show a
    //   frame's framebuffer before the game has drawn all of it. Mario Party's board, whose frames are three or four RSP
    //   tasks into one framebuffer, presented it after the first now and then, a few times a minute, and the frame ended
    //   there drew its 3D alone for a frame, without the characters, Toad, the HUD or a text box,
    // - before a workload that starts the next one: an offscreen pass (Workload::offscreenOnly), or one that draws to
    //   another framebuffer the VI shows, as the next frame of a game with two or three does, after one that has drawn,
    // - where something waits for one of its workloads (waitForWorkloadId(): a present whose workload is the next
    //   frame's offscreen pass, the setup's warm-up, a pause), once it has as many workloads that draw as the frame
    //   before had, after HeldFrameLimit, or once the game waits for a free present (presentWaiting()), as it can't
    //   queue the rest then: a present that shows the frame before can come in the
    //   middle of the next one, and a frame ended there was drawn in two, each part matched against the whole frame
    //   before, and its first part shown alone (the presentation's SkipBuffering shows a framebuffer as soon as a frame
    //   draws to it). Mario Party's board, whose frames are six RSP tasks and whose VI shows its frame now and then
    //   after four of the next one's, about every 0.4 s, drew Toad tiny and an object near him huge between its frames;
    //   the rest of each such frame came 16-18 ms after the wait on an idle host, and up to 260 ms after on a busy one,
    //   where its board flickered as the characters jumped out of the tube at its start. A frame of offscreen passes
    //   alone is held the same way, for the workload that draws what the VI shows: Jet Force Gemini's shadow pass, the
    //   next frame's, is queued before the present that waits for it on a fast host, and ended there as a frame of its
    //   own it took the interpolated frames' counters from the frame before with one present, and the next frame was
    //   matched against it: its levels showed no in-between frames, 60 presents a second uncapped where a slower host
    //   showed 210-350,
    // - where the queue is full, so the game never waits for a frame that can't end (WORKLOAD_QUEUE_SIZE),
    // - and a workload the debugger repeats while paused is a frame of its own, as every workload was before.
    // Workloads that draw nothing are part of the frame they are queued in. Called with cursorMutex held; end says why
    // the frame ended.
    uint32_t WorkloadQueue::endedGameFrame(int startCursor, GameFrameEnd &end) const {
        const uint32_t queueSize = uint32_t(workloads.size());
        uint32_t count = 0;
        bool drawsShown = false;
        uint32_t shownFbAddress = 0;
        bool waitedFor = false;
        bool offscreen = false;
        uint32_t drawnCount = 0;
        for (int c = startCursor; c != writeCursor; c = (c + 1) % queueSize) {
            const Workload &workload = workloads[c];
            if (workload.paused) {
                end = GameFrameEnd::Paused;
                return (count > 0) ? count : 1;
            }

            if (drawsShown && (workload.offscreenOnly || (workload.drawsShown && (workload.shownFbAddress != shownFbAddress)))) {
                end = GameFrameEnd::NextFrame;
                return count;
            }

            count++;
            if (workload.fbPairCount > 0) {
                drawnCount++;
            }

            offscreen = offscreen || workload.offscreenOnly;
            if (workload.drawsShown) {
                drawsShown = true;
                shownFbAddress = workload.shownFbAddress;
            }

            if (drawsShown) {
                for (const QueuedPresent &present : queuedPresents) {
                    if ((present.workloadId == workload.workloadId) && (present.fbAddress == shownFbAddress)) {
                        if (drawnCount >= lastFrameDrawnCount) {
                            end = GameFrameEnd::Present;
                            return count;
                        }

                        waitedFor = true;
                    }
                }
            }

            waitedFor = waitedFor || (workload.workloadId <= waitedWorkloadId);
        }

        const bool queueFull = ((writeCursor + 1) % queueSize) == uint32_t(barrierCursor);
        if (queueFull) {
            end = GameFrameEnd::QueueFull;
            return count;
        }

        if (!waitedFor) {
            return 0;
        }

        // A frame with fewer workloads that draw than the one before, or of offscreen passes alone, is held for the rest of
        // them, up to HeldFrameLimit or until the game waits for a free present.
        if ((drawsShown && (drawnCount < lastFrameDrawnCount)) || (!drawsShown && offscreen)) {
            const uint64_t firstWorkloadId = workloads[startCursor].workloadId;
            const auto now = std::chrono::steady_clock::now();
            if (heldWorkloadId != firstWorkloadId) {
                heldWorkloadId = firstWorkloadId;
                heldSince = now;
            }

            if (((now - heldSince) < HeldFrameLimit) && !presentWaiting()) {
                return 0;
            }

            end = GameFrameEnd::HeldLimit;
            return count;
        }

        end = GameFrameEnd::Wait;
        return count;
    }

    // Milliseconds since 1970, the clock RT64_FRAME_LOG's lines are stamped with.
    long long WorkloadQueue::logTime() {
        const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
        return (long long)(std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count());
    }

    // One line for the game frame, the time in milliseconds since 1970 (to line up with a capture of the window), why it
    // ended and how many of its workloads draw against the frame before's; then one for each of its workloads: whether
    // it draws to a framebuffer the VI shows, and which, or is an offscreen pass, and each framebuffer it draws to as
    // address/width. Called with cursorMutex held.
    void WorkloadQueue::logGameFrame(int startCursor, uint32_t count, GameFrameEnd end) const {
        static const char *EndNames[] = { "present", "next", "wait", "held", "full", "paused" };
        const long long ms = logTime();
        uint32_t drawnCount = 0;
        for (uint32_t i = 0; i < count; i++) {
            drawnCount += (workloads[(startCursor + i) % workloads.size()].fbPairCount > 0) ? 1 : 0;
        }

        fprintf(stderr, "GF t=%lld n=%u end=%s drawn=%u before=%u\n", ms, count, EndNames[uint32_t(end)], drawnCount, lastFrameDrawnCount);
        for (uint32_t i = 0; i < count; i++) {
            const Workload &workload = workloads[(startCursor + i) % workloads.size()];
            fprintf(stderr, "  WL %llu shown=%08X off=%d pairs=%u:", (unsigned long long)(workload.workloadId),
                workload.drawsShown ? workload.shownFbAddress : 0U, workload.offscreenOnly ? 1 : 0, workload.fbPairCount);
            for (uint32_t f = 0; f < workload.fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                fprintf(stderr, " %08X/%u%s", fbPair.colorImage.address, fbPair.colorImage.width, fbPair.drawColorRect.isEmpty() ? "/empty" : "");
            }

            fprintf(stderr, "\n");
        }

        fflush(stderr);
    }
#endif

    void WorkloadQueue::repeatLastWorkload() {
        {
            const std::scoped_lock lock(cursorMutex);
            threadCursor = previousWriteCursor();
        }

        cursorCondition.notify_all();
    }

    uint32_t WorkloadQueue::previousWriteCursor() const {
        if (writeCursor > 0) {
            return writeCursor - 1;
        }
        else {
            return uint32_t(workloads.size()) - 1;
        }
    }

    void WorkloadQueue::waitForIdle() {
        std::unique_lock<std::mutex> threadLock(threadMutex);
    }

    void WorkloadQueue::waitForWorkloadId(uint64_t waitId) {
#   if RT64_GAME_FRAME_WORKLOADS
        // The game frame with this workload in it ends where it is (endedGameFrame()).
        {
            const std::scoped_lock lock(cursorMutex);
            waitedWorkloadId = std::max(waitedWorkloadId, waitId);
            if (frameLog) {
                fprintf(stderr, "WAIT t=%lld wl=%llu\n", logTime(), (unsigned long long)(waitId));
            }
        }

        cursorCondition.notify_all();
#   endif

        std::unique_lock<std::mutex> workloadLock(workloadIdMutex);
        workloadIdCondition.wait(workloadLock, [&]() {
            return (waitId <= workloadId) || !threadsRunning;
        });
    }

    void WorkloadQueue::setup(const External &ext) {
        this->ext = ext;

#   if RT64_GAME_FRAME_WORKLOADS
        frameLog = (getenv("RT64_FRAME_LOG") != nullptr);
#       if defined(__EMSCRIPTEN__)
        // A page has no environment of its own: one opened with ?frame-log prints the log to the console.
        frameLog = frameLog || (MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).has('frame-log') ? 1 : 0; }) != 0);
#       endif
#   endif

        rspProcessor = std::make_unique<RSPProcessor>(ext.device);
        vertexProcessor = std::make_unique<VertexProcessor>(ext.device);
        framebufferRenderer = std::make_unique<FramebufferRenderer>(ext.workloadGraphicsWorker, true, ext.createdGraphicsAPI, ext.shaderLibrary);
        renderFramebufferManager = std::make_unique<RenderFramebufferManager>(ext.device);
        queryPool = ext.device->createQueryPool(2);

        projectionProcessor.setup(ext.workloadGraphicsWorker);
        transformProcessor.setup(ext.workloadGraphicsWorker);
        tileProcessor.setup(ext.workloadGraphicsWorker);
        lookAtProcessor.setup(ext.workloadGraphicsWorker);

        threadsRunning = true;
        renderThread = new std::thread(&WorkloadQueue::renderThreadLoop, this);
#ifndef __EMSCRIPTEN__
        // Not in a browser: it keeps a desktop driver from lowering the GPU's clock, which a page has no say in, and every
        // dispatch would be a trip to the thread that owns the WebGPU device.
        idleThread = new std::thread(&WorkloadQueue::idleThreadLoop, this);
#endif
    }

    void WorkloadQueue::updateMultisampling() {
        renderFramebufferManager->destroyAll();
        dummyDepthTarget.reset();
#   if RT64_WGSL
        dummyDepthTargets.dummies.clear();
#   endif
        framebufferRenderer->updateMultisampling();
    }

    void WorkloadQueue::threadConfigurationUpdate(hlslpp::uint2 viFbSize, WorkloadConfiguration &workloadConfig) {
        const std::scoped_lock lock(ext.sharedResources->configurationMutex);
        const bool sizeChanged = ext.sharedResources->swapChainSizeChanged;
        ext.sharedResources->swapChainSizeChanged = false;
        
        // Retrieve the reference height to be used for determining the resolution scale. Impose a minimum in case
        // the game is using too small of a portion of the VI.
        const uint32_t MinimumReferenceHeight = 60;
        const uint32_t referenceHeight = (viFbSize[1] > 0) ? std::max(viFbSize[1], MinimumReferenceHeight) : 240;

        // Compute the aspect ratio to be used for the frame.
        workloadConfig.aspectRatioSource = (viFbSize[1] > 0) ? float(viFbSize[0]) / float(viFbSize[1]) : (4.0f / 3.0f);

        const auto ratioMode = ext.sharedResources->userConfig.aspectRatio;
        switch (ratioMode) {
        case UserConfiguration::AspectRatio::Expand:
            if ((ext.sharedResources->swapChainWidth > 0) && (ext.sharedResources->swapChainHeight > 0)) {
                const float derivedRatioTarget = float(ext.sharedResources->swapChainWidth) / float(ext.sharedResources->swapChainHeight);
                workloadConfig.aspectRatioTarget = std::max(derivedRatioTarget, workloadConfig.aspectRatioSource);
            }
            else {
                workloadConfig.aspectRatioTarget = workloadConfig.aspectRatioSource;
            }

            break;
        case UserConfiguration::AspectRatio::Manual:
            workloadConfig.aspectRatioTarget = float(ext.sharedResources->userConfig.aspectTarget);
            break;
        case UserConfiguration::AspectRatio::Original:
        default:
            workloadConfig.aspectRatioTarget = workloadConfig.aspectRatioSource;
            break;
        }

        // Compute the extended GBI aspect ratio percentage to be used for the frame.
        const auto extRatioMode = ext.sharedResources->userConfig.extAspectRatio;
        switch (extRatioMode) {
        case UserConfiguration::AspectRatio::Expand:
            workloadConfig.extAspectPercentage = 1.0f;
            break;
        case UserConfiguration::AspectRatio::Manual:
            if ((ext.sharedResources->swapChainWidth > 0) && (ext.sharedResources->swapChainHeight > 0)) {
                const float reducedExtTarget = float(ext.sharedResources->userConfig.extAspectTarget) - workloadConfig.aspectRatioSource;
                const float reducedDisplayTarget = workloadConfig.aspectRatioTarget - workloadConfig.aspectRatioSource;
                if ((reducedExtTarget > 0.0f) && (reducedDisplayTarget > 0.0f)) {
                    workloadConfig.extAspectPercentage = std::clamp((reducedExtTarget / reducedDisplayTarget), 0.0f, 1.0f);
                }
                else {
                    workloadConfig.extAspectPercentage = 0.0f;
                }
            }
            else {
                workloadConfig.extAspectPercentage = 0.0f;
            }

            break;
        case UserConfiguration::AspectRatio::Original:
        default:
            workloadConfig.extAspectPercentage = 0.0f;
            break;
        }

        // Compute the resolution scaling to be used for the frame.
        float resolutionMultiplier;
        const auto resolutionMode = ext.sharedResources->userConfig.resolution;
        switch (resolutionMode) {
        case UserConfiguration::Resolution::WindowIntegerScale:
            if (ext.sharedResources->swapChainHeight > 0) {
                resolutionMultiplier = std::max(float((ext.sharedResources->swapChainHeight + referenceHeight - 1) / referenceHeight), 1.0f);
            }
            else {
                resolutionMultiplier = 1.0f;
            }

            break;
        case UserConfiguration::Resolution::Manual:
            resolutionMultiplier = float(ext.sharedResources->userConfig.resolutionMultiplier);
            break;
        case UserConfiguration::Resolution::Original:
        default:
            resolutionMultiplier = 1.0f;
            break;
        }

        uint32_t msaaSampleCount = ext.sharedResources->userConfig.msaaSampleCount();

        // Build the resolution scale vector from the configuration.
        workloadConfig.aspectRatioScale = workloadConfig.aspectRatioTarget / workloadConfig.aspectRatioSource;
        workloadConfig.resolutionScale = { resolutionMultiplier * workloadConfig.aspectRatioScale, resolutionMultiplier };
        workloadConfig.downsampleMultiplier = ext.sharedResources->userConfig.downsampleMultiplier;
        ext.sharedResources->resolutionScale = workloadConfig.resolutionScale;

        // Find the target refresh rate from the configuration.
        const auto refreshRate = ext.sharedResources->userConfig.refreshRate;
        switch (refreshRate) {
        case UserConfiguration::RefreshRate::Display:
            workloadConfig.targetRate = ext.sharedResources->swapChainRate;
            break;
        case UserConfiguration::RefreshRate::Manual:
            workloadConfig.targetRate = ext.sharedResources->userConfig.refreshRateTarget;

            // Limit the target rate to the rate detected by the swap chain.
            if ((ext.sharedResources->swapChainRate > 0) && (workloadConfig.targetRate > ext.sharedResources->swapChainRate)) {
                workloadConfig.targetRate = ext.sharedResources->swapChainRate;
            }

            break;
        case UserConfiguration::RefreshRate::Original:
        default:
            workloadConfig.targetRate = 0;
            break;
        }

        // Store the rate that was chosen for the configuration.
        ext.sharedResources->targetRate = workloadConfig.targetRate;

        // A lower rate the present queue paces the frames to while the GPU can't keep up with this one (PresentQueue::pace()).
        const uint32_t pacedRate = ext.sharedResources->pacedRate;
        if ((pacedRate > 0) && (pacedRate < workloadConfig.targetRate)) {
            workloadConfig.targetRate = pacedRate;
        }

#   if RT_ENABLED
        workloadConfig.raytracingEnabled = rtEnabled;

        if (workloadConfig.raytracingEnabled && (ext.sharedResources->rtConfigChanged || ext.sharedResources->fbConfigChanged || sizeChanged)) {
            // Only load the RT pipeline if the device supports it.
             if (ext.device->getCapabilities().raytracing && !ext.rtShaderCache->isSetup()) {
                 ext.rtShaderCache->setup();
            }

            framebufferRenderer->setRaytracingConfig(ext.sharedResources->rtConfig, ext.sharedResources->fbConfigChanged || sizeChanged);
            ext.sharedResources->rtConfigChanged = false;
        }
#   endif
        
        workloadConfig.postBlendNoise = ext.sharedResources->emulatorConfig.dither.postBlendNoise;
        workloadConfig.postBlendNoiseNegative = ext.sharedResources->emulatorConfig.dither.postBlendNoiseNegative;
        
        if (ext.sharedResources->fbConfigChanged || sizeChanged) {
            {
                // Wait until the other queue has stopped using the interpolated color targets.
                std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
#           if RT64_GAME_FRAME_WORKLOADS
                // While a present holds either set of counters (InterpolatedFrameCounters::inUse), not until the current set
                // has been presented: no present may ever show it, and the present that would can be waiting for this very
                // workload, so this waited for good, the present thread and the game behind it. Mario Party 2's page stopped
                // there at a window's resize now and then. Neither set keeps frames for a present to take after, from the
                // targets cleared below: one that takes either presents its frame without the interpolated ones.
                ext.sharedResources->interpolatedCondition.wait(interpolatedLock, [&]() {
                    return (!ext.sharedResources->interpolatedFrames[0].inUse && !ext.sharedResources->interpolatedFrames[1].inUse) || !threadsRunning;
                });

                for (InterpolatedFrameCounters &counters : ext.sharedResources->interpolatedFrames) {
                    counters.available = 0;
                    counters.presented = counters.count;
                    counters.skipped = true;
                }
#           else
                InterpolatedFrameCounters &curFrameCounters = ext.sharedResources->interpolatedFrames[ext.sharedResources->interpolatedFramesIndex];
                ext.sharedResources->interpolatedCondition.wait(interpolatedLock, [&]() {
                    return curFrameCounters.presented >= curFrameCounters.available;
                });
#           endif
            }

            std::scoped_lock<std::mutex> managerLock(ext.sharedResources->managerMutex);
            FramebufferManager &fbManager = ext.sharedResources->framebufferManager;
            RenderTargetManager &targetManager = ext.sharedResources->renderTargetManager;
            renderFramebufferManager->destroyAll();
            targetManager.destroyAll();
            fbManager.destroyAllTileCopies();
            ext.sharedResources->fbConfigChanged = false;
            ext.sharedResources->interpolatedColorTargets.clear();
        }

        if (ext.sharedResources->userConfigChanged) {
            idleMutex.lock();
            idleActive = ext.sharedResources->userConfig.idleWorkActive;
            idleMutex.unlock();
            idleCondition.notify_all();
        }
    }

    void WorkloadQueue::threadConfigurationValidate() {
        const std::scoped_lock lock(ext.sharedResources->configurationMutex);
        if (ext.sharedResources->userConfigChanged) {
            ext.sharedResources->newConfigValidated = true;
            ext.sharedResources->userConfigChanged = false;
        }
    }
    
    void WorkloadQueue::threadRenderFrame(GameFrame &curFrame, const GameFrame &prevFrame, const WorkloadConfiguration &workloadConfig,
        const DebuggerRenderer &debuggerRenderer, const DebuggerCamera &debuggerCamera, float curFrameWeight, float prevFrameWeight,
        float deltaTimeMs, RenderTargetKey overrideTargetKey, uint32_t overrideTargetWorkload, int32_t overrideTargetFbPairIndex, RenderTarget *overrideTarget,
        uint32_t overrideTargetModifier, bool uploadVelocity, bool uploadExtras, bool interpolateTiles, bool interpolateLookAts)
    {
#   if ENABLE_HIGH_RESOLUTION_RENDERER
        std::scoped_lock<std::mutex> managerLock(ext.sharedResources->workloadMutex);
        FramebufferManager &fbManager = ext.sharedResources->framebufferManager;
        RenderTargetManager &targetManager = ext.sharedResources->renderTargetManager;
        const bool usingMSAA = (targetManager.multisampling.sampleCount > 1);

        rendererCPUProfiler.start();

        const bool aspectRatioAdjustment = (abs(workloadConfig.aspectRatioScale - 1.0f) > 1e-6f);
        const bool processProjections = aspectRatioAdjustment || prevFrame.matched|| curFrame.isDebuggerCameraEnabled(*this);
        bool uploadProjections = false;
        if (processProjections) {
            ProjectionProcessor::ProcessParams projParams;
            projParams.worker = ext.workloadGraphicsWorker;
            projParams.workloadQueue = this;
            projParams.curFrame = &curFrame;
            projParams.prevFrame = &prevFrame;
            projParams.curFrameWeight = curFrameWeight;
            projParams.prevFrameWeight = prevFrameWeight;
            projParams.aspectRatioScale = workloadConfig.aspectRatioScale;
            projectionProcessor.process(projParams);
            projectionProcessor.upload(projParams);
            uploadProjections = true;
        }

        const bool processTransforms = prevFrame.matched;
        bool uploadTransforms = false;
        if (processTransforms) {
            TransformProcessor::ProcessParams transformParams;
            transformParams.worker = ext.workloadGraphicsWorker;
            transformParams.workloadQueue = this;
            transformParams.curFrame = &curFrame;
            transformParams.prevFrame = &prevFrame;
            transformParams.curFrameWeight = curFrameWeight;
            transformParams.prevFrameWeight = prevFrameWeight;
            transformProcessor.process(transformParams);
            transformProcessor.upload(transformParams);
            uploadTransforms = true;
        }

        bool uploadTiles = false;
        if (interpolateTiles) {
            TileProcessor::ProcessParams tileParams;
            tileParams.worker = ext.workloadGraphicsWorker;
            tileParams.workloadQueue = this;
            tileParams.curFrame = &curFrame;
            tileParams.prevFrame = &prevFrame;
            tileParams.curFrameWeight = curFrameWeight;
            tileParams.prevFrameWeight = prevFrameWeight;
            tileProcessor.process(tileParams);
            tileProcessor.upload(tileParams);
            uploadTiles = true;
        }

        bool uploadLookAts = false;
        if (interpolateLookAts) {
            LookAtProcessor::ProcessParams lookAtParams;
            lookAtParams.worker = ext.workloadGraphicsWorker;
            lookAtParams.workloadQueue = this;
            lookAtParams.curFrame = &curFrame;
            lookAtParams.prevFrame = &prevFrame;
            lookAtParams.curFrameWeight = curFrameWeight;
            lookAtParams.prevFrameWeight = prevFrameWeight;
            lookAtProcessor.process(lookAtParams);
            lookAtProcessor.upload(lookAtParams);
            uploadLookAts = true;
        }

        // Reset the max height tracking for all active framebuffers.
        fbManager.resetTracking();

        if ((overrideTarget != nullptr) && !usingMSAA) {
            targetManager.setOverride(overrideTargetKey, overrideTarget);
        }

        for (uint32_t w = 0; w < curFrame.workloads.size(); w++) {
            Workload &workload = workloads[curFrame.workloads[w]];

            // There's no guarantee the RSP was processed if framebuffers were not rendered.
            const bool processRSP = true;
            if (processRSP) {
                workload.resetRSPOutputBuffers();

                RSPProcessor::ProcessParams rspParams;
                rspParams.worker = ext.workloadGraphicsWorker;
                rspParams.drawData = &workload.drawData;
                rspParams.drawBuffers = &workload.drawBuffers;
                rspParams.outputBuffers = &workload.outputBuffers;
                rspParams.prevFrameWeight = prevFrameWeight;
                rspParams.curFrameWeight = curFrameWeight;
#           if RT64_PACKED_RSP_INPUTS
                rspParams.packedSlot = w;
#           endif
                rspProcessor->process(rspParams);
            }

            const bool processWorldVertices = prevFrame.matched;
            if (processWorldVertices) {
                workload.resetWorldOutputBuffers();

                VertexProcessor::ProcessParams vertexParams;
                vertexParams.worker = ext.workloadGraphicsWorker;
                vertexParams.drawData = &workload.drawData;
                vertexParams.drawBuffers = &workload.drawBuffers;
                vertexParams.outputBuffers = &workload.outputBuffers;
                vertexParams.curFrameWeight = curFrameWeight;
                vertexParams.prevFrameWeight = prevFrameWeight;
#           if RT64_PACKED_RSP_INPUTS
                vertexParams.packedSlot = w;
#           endif
                vertexProcessor->process(vertexParams);
            }

            hlslpp::float2 fixedResScale;
            Framebuffer *colorFb;
            Framebuffer *depthFb;
            uint32_t nativeColorWidth;
            uint32_t nativeColorHeight;
            uint32_t targetWidth;
            uint32_t targetHeight;
            uint32_t targetMisalignX;
            uint32_t rtWidth;
            uint32_t rtHeight;
            RenderTarget *colorTarget;
            RenderTarget *depthTarget;
            RenderFramebufferKey fbKey;
            auto getTargetsFromPair = [&](uint32_t f) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                const auto &colorImg = fbPair.colorImage;
                const auto &depthImg = fbPair.depthImage;
                fixedResScale = workloadConfig.resolutionScale;
                if (!fbPair.drawColorRect.isEmpty()) {
                    colorFb = nullptr;
                    depthFb = nullptr;
                    nativeColorWidth = colorImg.width;
                    nativeColorHeight = fbPair.drawColorRect.bottom(true);

                    // The height becomes the target's, and RasterVS's clip space is built from it, so its bottom clips.
                    // drawColorRect is fitted to where this game frame put the triangles, but an interpolated frame puts
                    // them between the last game frame's place and this one's, and can put them lower: with the camera
                    // pitching down, a band at the bottom was cut off while the framebuffer's height hadn't yet reached
                    // it. The height is at least what the viewports the triangles were drawn through cover inside their
                    // scissors, which the game sets and interpolation doesn't move. Not the scissor alone: it starts at
                    // 8192 lines, and a game can draw to a small framebuffer under a screen-sized one.
                    if (!fbPair.viewportRect.isNull()) {
                        nativeColorHeight = std::max(nativeColorHeight, uint32_t(std::max(0, fbPair.viewportRect.bottom(true))));
                    }

                    // When the target is much bigger than the reference height, we reduce the resolution scaling (but clamped to 1.0).
                    const uint32_t heightThreshold = (workload.viFbSize[1] > 0) ? ((workload.viFbSize[1] * 3) / 2) : 360;
                    uint32_t downsampleMultiplier = workloadConfig.downsampleMultiplier;
                    if ((nativeColorHeight >= heightThreshold) && (fixedResScale[1] >= 2.0f)) {
                        fixedResScale = hlslpp::max(fixedResScale / 2.0f, hlslpp::float2(1.0f, 1.0f));
                        downsampleMultiplier = std::max(downsampleMultiplier / 2U, 1U);
                    }

                    if (fbPair.depthRead || fbPair.depthWrite || fbPair.fastPaths.clearDepthOnly) {
                        uint32_t depthAddress = fbPair.fastPaths.clearDepthOnly ? colorImg.address : depthImg.address;
                        depthFb = &fbManager.get(depthAddress, G_IM_SIZ_16b, nativeColorWidth, nativeColorHeight);
                        depthFb->everUsedAsDepth = true;
                    }
                    else {
                        depthFb = nullptr;
                    }

                    // Ensure dimensions are the same for the color and depth targets based on their previous sizes.
                    fbKey = RenderFramebufferKey();

                    if (!fbPair.fastPaths.clearDepthOnly) {
                        colorFb = &fbManager.get(colorImg.address, colorImg.siz, nativeColorWidth, nativeColorHeight);
                    }

                    if (colorFb != nullptr) {
                        fbKey.colorTargetKey = RenderTargetKey(colorFb->addressStart, colorFb->width, colorFb->siz, Framebuffer::Type::Color);
                        colorTarget = &targetManager.get(fbKey.colorTargetKey);
                    }
                    else {
                        colorTarget = nullptr;
                    }

                    // Apply the modifier key if we retrieved the override target.
                    if ((colorTarget != nullptr) && (colorTarget == overrideTarget)) {
                        fbKey.modifierKey = overrideTargetModifier;
                    }

                    fixedResScale = RenderTarget::computeFixedResolutionScale(colorImg.width, fixedResScale);
                    RenderTarget::computeScaledSize(nativeColorWidth, nativeColorHeight, fixedResScale, targetWidth, targetHeight, targetMisalignX);

                    rtWidth = targetWidth;
                    rtHeight = targetHeight;

                    // The desired size should not be less than the existing size of the color and depth targets.
                    RenderTarget *chosenRt = nullptr;
                    if (depthFb != nullptr) {
                        fbKey.depthTargetKey = RenderTargetKey(depthFb->addressStart, depthFb->width, depthFb->siz, Framebuffer::Type::Depth);
                        depthTarget = &targetManager.get(fbKey.depthTargetKey);
                        depthTarget->resolutionScale = fixedResScale;
                        rtWidth = std::max(rtWidth, depthTarget->width);
                        rtHeight = std::max(rtHeight, depthTarget->height);
                        chosenRt = depthTarget;
                    }
                    else {
                        depthTarget = nullptr;
                    }

                    if (colorTarget != nullptr) {
                        rtWidth = std::max(rtWidth, colorTarget->width);
                        rtHeight = std::max(rtHeight, colorTarget->height);
                        chosenRt = colorTarget;
                    }

                    assert(chosenRt != nullptr);
                    chosenRt->resolutionScale = fixedResScale;
                    chosenRt->downsampleMultiplier = downsampleMultiplier;
                    chosenRt->misalignX = targetMisalignX;
                    chosenRt->invMisalignX = (targetMisalignX > 0) ? (std::lround(fixedResScale.y) - targetMisalignX) : 0;

                    assert((colorTarget != nullptr) || (depthTarget != nullptr));
                    return true;
                }
                else {
                    return false;
                }
            };

            thread_local std::unordered_set<RenderTarget *> resizedTargets;
            thread_local std::vector<std::pair<RenderTarget *, RenderTarget *>> colorDepthPairs;
            resizedTargets.clear();
            colorDepthPairs.clear();

            const uint32_t fbPairCount = (debuggerRenderer.framebufferIndex >= 0) ? (debuggerRenderer.framebufferIndex + 1) : workload.fbPairCount;
            for (uint32_t f = 0; f < fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
#           if RT_ENABLED
                for (uint32_t p = 0; p < fbPair.projectionCount; p++) {
                    const Projection &proj = fbPair.projections[p];
                    const bool perspProj = (proj.type == Projection::Type::Perspective);
                    const bool rtProj = (perspProj && workloadConfig.raytracingEnabled && fbPair.depthWrite); // TODO: Move this condition out of here, ideally by moving the shader submission elsewhere.
                    if (!rtProj) {
                        continue;
                    }

                    // Submit RT shaders if it's an RT proj.
                    for (uint32_t d = 0; d < proj.gameCallCount; d++) {
                        const GameCall &call = proj.gameCalls[d];
                        ext.rtShaderCache->submit(call.shaderDesc);
                    }
                }
#           endif

                // Resize the render targets for this framebuffer pair if necessary.
                if (getTargetsFromPair(f)) {
                    // Resize the native target buffers.
                    if (colorFb != nullptr) {
                        colorFb->nativeTarget.resetBufferHistory();
                    }

                    if (depthFb != nullptr) {
                        depthFb->nativeTarget.resetBufferHistory();
                    }

                    if ((colorTarget != nullptr) && colorTarget->resize(ext.workloadGraphicsWorker, rtWidth, rtHeight)) {
                        resizedTargets.emplace(colorTarget);
                        colorFb->readHeight = 0;
                    }

                    // Set up the dummy target used for rendering the depth if no depth framebuffer is active. WebGPU's
                    // is one of its color target's size, got as it draws (DummyDepthTargets).
                    if (depthFb == nullptr) {
#                   if !RT64_WGSL
                        if (dummyDepthTarget == nullptr) {
                            dummyDepthTarget = std::make_unique<RenderTarget>(0, Framebuffer::Type::Depth, targetManager.multisampling, targetManager.usesHDR);
                            dummyDepthTarget->setupDepth(ext.workloadGraphicsWorker, rtWidth, rtHeight);
                        }

                        if ((dummyDepthTarget != nullptr) && dummyDepthTarget->resize(ext.workloadGraphicsWorker, rtWidth, rtHeight)) {
                            resizedTargets.emplace(dummyDepthTarget.get());
                        }

                        if (colorTarget != nullptr) {
                            colorDepthPairs.emplace_back(colorTarget, dummyDepthTarget.get());
                        }
#                   endif
                    }
                    else if (depthTarget != nullptr) {
                        if (colorTarget != nullptr) {
                            colorDepthPairs.emplace_back(colorTarget, depthTarget);
                        }

                        if (depthTarget->resize(ext.workloadGraphicsWorker, rtWidth, rtHeight)) {
                            resizedTargets.emplace(depthTarget);
                            depthFb->readHeight = 0;
                        }
                    }
                }

                fbManager.setupOperations(ext.workloadGraphicsWorker, fbPair.startFbOperations, fixedResScale, targetManager, &resizedTargets);
                fbManager.setupOperations(ext.workloadGraphicsWorker, fbPair.endFbOperations, fixedResScale, targetManager, &resizedTargets);
            }

            // Make sure all depth targets are at least bigger than their corresponding color targets.
            for (auto colorDepthPair : colorDepthPairs) {
                if (colorDepthPair.second->resize(ext.workloadGraphicsWorker, colorDepthPair.first->width, colorDepthPair.first->height)) {
                    resizedTargets.emplace(colorDepthPair.second);
                }
            }

#       if RT64_WGSL
            RenderTarget::matchColorToDepthTargets(ext.workloadGraphicsWorker, colorDepthPairs, resizedTargets);
#       endif

            for (RenderTarget *renderTarget : resizedTargets) {
                renderFramebufferManager->destroyAllWithRenderTarget(renderTarget);
            }

            uint32_t gameCallCursor = 0;
            const uint32_t gameCallCountMax = (debuggerRenderer.globalDrawCallIndex >= 0) ? (debuggerRenderer.globalDrawCallIndex + 1) : workload.gameCallCount;
            thread_local std::vector<BufferUploader *> bufferUploaders;
            bufferUploaders.clear();

            // Indicate to the texture cache the textures must not be deleted.
            ext.textureCache->incrementLock();

            // Reset the texture cache vectors for the framebuffer renderer.
            framebufferRenderer->updateTextureCache(ext.textureCache);

            for (uint32_t f = 0; f < fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                fbManager.performDiscards(fbPair.startFbDiscards);
#           if RT64_FREE_DISCARDED_TARGETS
                targetManager.discard(fbPair.startFbDiscards, workload.submissionFrame);
#           endif
            }
            
            // Add all framebuffer pairs to the framebuffer renderer and setup the operations.
            scratchFbChangePool.reset();
            fbManager.resetOperations();
            framebufferRenderer->resetFramebuffers(ext.workloadGraphicsWorker, ubershadersVisible, workload.extended.ditherNoiseStrength, targetManager.multisampling);

#       if RT_ENABLED
            if (workloadConfig.raytracingEnabled) {
                framebufferRenderer->resetRaytracing(ext.rtShaderCache, ext.blueNoiseTexture);
            }
#       endif

            for (uint32_t f = 0; f < fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                if (getTargetsFromPair(f)) {
#               if RT64_WGSL
                    RenderTarget *pairDepthTarget = (depthTarget != nullptr) ? depthTarget : dummyDepthTargets.get(ext.workloadGraphicsWorker, colorTarget, workload.submissionFrame);
#               else
                    RenderTarget *pairDepthTarget = (depthTarget != nullptr) ? depthTarget : dummyDepthTarget.get();
#               endif
                    RenderFramebufferStorage &fbStorage = renderFramebufferManager->get(fbKey, colorTarget, pairDepthTarget);
                    FramebufferRenderer::DrawParams drawParams;
                    drawParams.worker = ext.workloadGraphicsWorker;
                    drawParams.fbStorage = &fbStorage;
                    drawParams.curWorkload = &workload;
                    drawParams.fbPairIndex = f;
                    drawParams.fbWidth = nativeColorWidth;
                    drawParams.fbHeight = nativeColorHeight;
                    drawParams.targetWidth = targetWidth;
                    drawParams.targetHeight = targetHeight;
                    drawParams.rasterShaderCache = ext.rasterShaderCache;
                    drawParams.resolutionScale = fixedResScale;
                    drawParams.aspectRatioSource = workloadConfig.aspectRatioSource;
                    drawParams.aspectRatioTarget = workloadConfig.aspectRatioTarget;
                    drawParams.extAspectPercentage = workloadConfig.extAspectPercentage;
                    drawParams.horizontalMisalignment = (colorTarget != nullptr) ? float(colorTarget->misalignX) : float(depthTarget->misalignX);
                    drawParams.presetScene = curFrame.presetScene;
                    drawParams.rtEnabled = workloadConfig.raytracingEnabled;
                    drawParams.submissionFrame = workload.submissionFrame;
                    drawParams.deltaTimeMs = deltaTimeMs;
                    drawParams.ubershadersOnly = ubershadersOnly || workload.extended.pipelineWarmUp;
                    drawParams.drawsNothing = workload.extended.pipelineWarmUp;
                    drawParams.postBlendNoise = workloadConfig.postBlendNoise;
                    drawParams.postBlendNoiseNegative = workloadConfig.postBlendNoiseNegative;
                    drawParams.maxGameCall = std::min(gameCallCountMax - gameCallCursor, fbPair.gameCallCount);
                    framebufferRenderer->addFramebuffer(drawParams);
                }
                
                gameCallCursor += fbPair.gameCallCount;
            }

            // Create all GPU tile mappings and upload them.
            if (!workload.drawData.gpuTiles.empty()) {
                std::pair<size_t, size_t> gpuTileRange;
                gpuTileRange.first = 0;
                gpuTileRange.second = workload.drawData.gpuTiles.size();
                framebufferRenderer->createGPUTiles(workload.drawData.callTiles.data(), uint32_t(workload.drawData.gpuTiles.size()),
                    workload.drawData.gpuTiles.data(), &fbManager, ext.textureCache, workload.submissionFrame);

                // Upload the GPU tiles.
                ext.workloadTilesUploader->submit(ext.workloadGraphicsWorker, {
                    { workload.drawData.gpuTiles.data(), gpuTileRange, sizeof(interop::GPUTile), RenderBufferFlag::STORAGE, { }, &workload.drawBuffers.gpuTilesBuffer}
                });

                bufferUploaders.emplace_back(ext.workloadTilesUploader);
            }

            if (uploadVelocity) {
                bufferUploaders.emplace_back(ext.workloadVelocityUploader);
                uploadVelocity = false;
            }

            if (uploadExtras) {
                bufferUploaders.emplace_back(ext.workloadExtrasUploader);
                uploadExtras = false;
            }

            if (uploadProjections) {
                bufferUploaders.emplace_back(projectionProcessor.bufferUploader.get());
                uploadProjections = false;
            }

            if (uploadTransforms) {
                bufferUploaders.emplace_back(transformProcessor.bufferUploader.get());
                uploadTransforms = false;
            }

            if (uploadTiles) {
                bufferUploaders.emplace_back(tileProcessor.bufferUploader.get());
                uploadTiles = false;
            }

            if (uploadLookAts) {
                bufferUploaders.emplace_back(lookAtProcessor.bufferUploader.get());
                uploadLookAts = false;
            }

#       if RT_ENABLED
            if (workloadConfig.raytracingEnabled) {
                ext.rtShaderCache->setNextState();
            }
#       endif

            workerMutex.lock();
            ext.workloadGraphicsWorker->commandList->begin();
            ext.workloadGraphicsWorker->commandList->resetQueryPool(queryPool.get(), 0, 2);
            ext.workloadGraphicsWorker->commandList->writeTimestamp(queryPool.get(), 0);
            framebufferRenderer->endFramebuffers(ext.workloadGraphicsWorker, &workload.drawBuffers, &workload.outputBuffers, workload.drawData.gpuTiles, workload.drawData.rdpTiles, workloadConfig.raytracingEnabled);
            framebufferRenderer->recordSetup(ext.workloadGraphicsWorker, bufferUploaders, processRSP ? rspProcessor.get() : nullptr, processWorldVertices ? vertexProcessor.get() : nullptr, &workload.outputBuffers, workloadConfig.raytracingEnabled);
            
            // Record all framebuffer pairs.
            uint32_t framebufferIndex = 0;
            for (uint32_t f = 0; f < fbPairCount; f++) {
                const FramebufferPair &fbPair = workload.fbPairs[f];
                bool validTargets = getTargetsFromPair(f);
                fbManager.recordOperations(ext.workloadGraphicsWorker, &workload.fbChangePool, &workload.fbStorage, ext.shaderLibrary, ext.textureCache,
                    fbPair.startFbOperations, targetManager, fixedResScale, f, workload.submissionFrame);

                if (validTargets) {
                    const auto &colorImg = fbPair.colorImage;
                    const auto &depthImg = fbPair.depthImage;
                    bool colorFormatUpdated = false;
                    if (colorFb != nullptr) {
                        if (colorImg.formatChanged) {
                            colorFb->discardLastWrite();
                        }
                        else if (colorFb->isLastWriteDifferent(Framebuffer::Type::Color)) {
                            RenderTargetKey otherColorTargetKey(colorFb->addressStart, colorFb->width, colorFb->siz, colorFb->lastWriteType);
                            RenderTarget &otherColorTarget = targetManager.get(otherColorTargetKey);
                            if (!otherColorTarget.isEmpty()) {
                                const FixedRect &r = colorFb->lastWriteRect;
                                colorTarget->copyFromTarget(ext.workloadGraphicsWorker, &otherColorTarget, r.left(false), r.top(false), r.width(false, true), r.height(false, true), ext.shaderLibrary);
                                colorFb->discardLastWrite();
                                colorFormatUpdated = true;
                            }
                        }

                        if (colorImg.formatChanged) {
                            colorTarget->clearColorTarget(ext.workloadGraphicsWorker);
                            colorFb->readHeight = 0;
                        }

                        if (colorFb->height > colorFb->readHeight) {
                            uint32_t readRowCount = colorFb->height - colorFb->readHeight;
                            FramebufferChange *colorFbChange = colorFb->readChangeFromStorage(ext.workloadGraphicsWorker, workload.fbStorage, scratchFbChangePool,
                                Framebuffer::Type::Color, colorImg.fmt, f, colorFb->readHeight, readRowCount, ext.shaderLibrary);

                            if (colorFbChange != nullptr) {
                                colorTarget->copyFromChanges(ext.workloadGraphicsWorker, *colorFbChange, colorFb->width, readRowCount, colorFb->readHeight, ext.shaderLibrary);
                            }

                            colorFb->readHeight = colorFb->height;
                        }
                    }

                    bool depthFormatUpdated = false;
                    bool depthFbChanged = false;
                    bool depthFbTypeChanged = false;
                    if (depthFb != nullptr) {
                        depthFbTypeChanged = (depthFb->lastWriteType == Framebuffer::Type::Color);

                        bool imgFormatChanged = (colorFb == nullptr) ? colorImg.formatChanged : depthImg.formatChanged;
                        if (imgFormatChanged) {
                            depthFb->discardLastWrite();
                        }
                        else if (depthFb->isLastWriteDifferent(Framebuffer::Type::Depth)) {
                            RenderTargetKey otherDepthTargetKey(depthFb->addressStart, depthFb->width, depthFb->siz, depthFb->lastWriteType);
                            RenderTarget &otherDepthTarget = targetManager.get(otherDepthTargetKey);
                            if (!otherDepthTarget.isEmpty()) {
                                const FixedRect &r = depthFb->lastWriteRect;
                                depthTarget->copyFromTarget(ext.workloadGraphicsWorker, &otherDepthTarget, r.left(false), r.top(false), r.width(false, true), r.height(false, true), ext.shaderLibrary);
                                depthFb->discardLastWrite();
                                depthFormatUpdated = true;
                            }
                        }

                        if (imgFormatChanged) {
                            depthTarget->clearDepthTarget(ext.workloadGraphicsWorker);
                            depthFb->readHeight = 0;
                        }

                        if (depthFb->height > depthFb->readHeight) {
                            uint32_t readRowCount = depthFb->height - depthFb->readHeight;
                            FramebufferChange *depthFbChange = depthFb->readChangeFromStorage(ext.workloadGraphicsWorker, workload.fbStorage, scratchFbChangePool, Framebuffer::Type::Depth,
                                G_IM_FMT_DEPTH, f, depthFb->readHeight, readRowCount, ext.shaderLibrary);

                            if (depthFbChange != nullptr) {
                                depthTarget->copyFromChanges(ext.workloadGraphicsWorker, *depthFbChange, depthFb->width, readRowCount, depthFb->readHeight, ext.shaderLibrary);
                                depthFbChanged = true;
                            }

                            depthFb->readHeight = depthFb->height;
                        }
                    }
                    
                    framebufferRenderer->recordFramebuffer(ext.workloadGraphicsWorker, framebufferIndex++);

                    // Transition the render targets in case the present queue will show them so it doesn't have to perform transitions.
                    if (colorTarget != nullptr && depthTarget != nullptr) {
                        RenderTextureBarrier textureBarriers[] = {
                            RenderTextureBarrier(colorTarget->texture.get(), RenderTextureLayout::SHADER_READ),
                            RenderTextureBarrier(depthTarget->texture.get(), RenderTextureLayout::SHADER_READ)
                        };

                        ext.workloadGraphicsWorker->commandList->barriers(RenderBarrierStage::GRAPHICS, textureBarriers, uint32_t(std::size(textureBarriers)));
                    }
                    else {
                        RenderTarget *chosenTarget = (colorTarget != nullptr) ? colorTarget : depthTarget;
                        ext.workloadGraphicsWorker->commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(chosenTarget->texture.get(), RenderTextureLayout::SHADER_READ));
                    }

                    // Do the resolve if using MSAA while target override is active and we're on the correct framebuffer pair index.
                    if (usingMSAA && (overrideTarget != nullptr) && (w == overrideTargetWorkload) && ((uint32_t)overrideTargetFbPairIndex == f)) {
                        overrideTarget->resize(ext.workloadGraphicsWorker, colorTarget->width, colorTarget->height);
                        overrideTarget->resolveFromTarget(ext.workloadGraphicsWorker, colorTarget, ext.shaderLibrary);
                    }

                    const uint64_t writeTimestamp = fbManager.nextWriteTimestamp();
                    FixedRect depthFbRect;
                    if (colorFb != nullptr) {
                        colorFb->lastWriteRect.merge(fbPair.drawColorRect.scaled(fixedResScale.x, fixedResScale.y));
                        colorFb->lastWriteType = Framebuffer::Type::Color;
                        colorFb->lastWriteFmt = colorImg.fmt;
                        colorFb->lastWriteTimestamp = writeTimestamp;
                        depthFbRect = fbPair.drawDepthRect;
                    }
                    else {
                        depthFbRect = fbPair.drawColorRect;
                    }
                    
                    const bool depthWrite = ((colorFb == nullptr) || depthFbChanged || depthFbTypeChanged || fbPair.depthWrite) && (depthFb != nullptr);
                    if (depthWrite && !depthFbRect.isNull()) {
                        depthFb->lastWriteRect.merge(depthFbRect.scaled(fixedResScale.x, fixedResScale.y));
                        depthFb->lastWriteType = Framebuffer::Type::Depth;
                        depthFb->lastWriteFmt = G_IM_FMT_DEPTH;
                        depthFb->lastWriteTimestamp = writeTimestamp;
                    }
                }
                
                fbManager.recordOperations(ext.workloadGraphicsWorker, &workload.fbChangePool, &workload.fbStorage, ext.shaderLibrary, ext.textureCache,
                    fbPair.endFbOperations, targetManager, fixedResScale, f, workload.submissionFrame);
            }

            ext.workloadGraphicsWorker->commandList->writeTimestamp(queryPool.get(), 1);
            ext.workloadGraphicsWorker->commandList->end();
            framebufferRenderer->waitForUploaders();
            ext.workloadGraphicsWorker->execute();
            ext.workloadGraphicsWorker->wait();

#       if RT64_WGSL
            dummyDepthTargets.freeUnused(workload.submissionFrame, renderFramebufferManager.get());
#       endif

            workerMutex.unlock();

            // Update the GPU profiler with the results from the timestamps of the frame.
#       if !defined(__EMSCRIPTEN__)
            // Not on WebGPU, which has no timestamps (plume's queries there do nothing and read zeros), and where behind
            // plume's device thread each of the two calls waits for a round trip to the page's thread after every workload.
            queryPool->queryResults();
            const uint64_t *frameTimestamps = queryPool->getResults();
            rendererGPUProfiler.log(double(frameTimestamps[1] - frameTimestamps[0]) / 1000000.0);
#       endif

            // Indicate to the texture cache it's safe to delete the textures if no locks are active.
            ext.textureCache->decrementLock();

#       if RT64_FREE_DISCARDED_TARGETS
            // Free the discarded targets no workload asked for again in as many workloads as the texture cache keeps
            // a texture nothing used, long after any workload or present that could still hold one.
            targetManager.freeDiscarded(workload.submissionFrame, WORKLOAD_QUEUE_SIZE * 32, renderFramebufferManager.get());
#       endif
        }

        if ((overrideTarget != nullptr) && !usingMSAA) {
            targetManager.removeOverride(overrideTargetKey);
        }

        framebufferRenderer->advanceFrame(workloadConfig.raytracingEnabled);
        rendererCPUProfiler.end();
        rendererCPUProfiler.log();
        rendererCPUProfiler.reset();
#   endif
    }

    void WorkloadQueue::threadAdvanceBarrier() {
        std::scoped_lock<std::mutex> cursorLock(cursorMutex);
        barrierCursor = (barrierCursor + 1) % workloads.size();
    }

    void WorkloadQueue::threadAdvanceWorkloadId(uint64_t newWorkloadId) {
        {
            std::scoped_lock<std::mutex> cursorLock(workloadIdMutex);
            workloadId = newWorkloadId;
        }

        workloadIdCondition.notify_all();
    }

    bool WorkloadQueue::isWorkloadDone(uint64_t id) {
        const std::scoped_lock lock(workloadIdMutex);
        return (id <= workloadId);
    }

    void WorkloadQueue::renderThreadLoop() {
        Thread::setCurrentThreadName("RT64 Workload");

        WorkloadConfiguration workloadConfig;
        int64_t logicalTicks = 0;
        int64_t displayTicks = 0;
        uint32_t originalRateForTicks = 0;
        uint32_t displayRateForTicks = 0;
        int processCursor = -1;
        bool frameReduction = false;
        uint32_t frameWorkloadCount = 0;
        while (threadsRunning) {
            {
                // With RT64_GAME_FRAME_WORKLOADS the workloads of a game frame are drawn together once it has ended
                // (endedGameFrame()): matched against the frame before as a whole, and each drawn again for every
                // interpolated frame. Otherwise each workload is a frame, and a frame drawn in several matched nothing
                // and wasn't interpolated, or each workload's interpolated frames drew only its own part over the last
                // state of the workloads before it.
                std::unique_lock<std::mutex> cursorLock(cursorMutex);
#           if RT64_GAME_FRAME_WORKLOADS
                // Woken by a new workload or a wait, and while a frame is held (endedGameFrame()) by its limit too.
                GameFrameEnd frameEnd = GameFrameEnd::Wait;
                while (threadsRunning) {
                    frameWorkloadCount = endedGameFrame(threadCursor, frameEnd);
                    if (frameWorkloadCount > 0) {
                        break;
                    }

                    cursorCondition.wait_for(cursorLock, std::chrono::milliseconds(2));
                }
#           else
                cursorCondition.wait(cursorLock, [&]() {
                    frameWorkloadCount = (writeCursor != threadCursor) ? 1 : 0;
                    return (frameWorkloadCount > 0) || !threadsRunning;
                });
#           endif

                if (threadsRunning) {
#               if RT64_GAME_FRAME_WORKLOADS
                    if (frameLog) {
                        logGameFrame(threadCursor, frameWorkloadCount, frameEnd);
                    }

                    // A frame after one cut short that draws to the same framebuffer the VI shows is the rest of it, and the
                    // two count as one game frame, if that framebuffer isn't the game frame before's: a game that draws each
                    // frame into the one framebuffer would add up every frame cut at a wait. Counted alone, the rest set how
                    // many the next frame needed before a wait could end it: Mario Party 2's page, whose present of a frame
                    // waits on the next frame's first workload, drew Tile Driver's frames in two from the first one cut,
                    // the cut at the frame before's count and each half the next one's, its boxes and PAUSE window missing
                    // every other frame.
                    uint32_t drawnCount = 0;
                    uint32_t shownFbAddress = 0;
                    for (uint32_t i = 0; i < frameWorkloadCount; i++) {
                        const Workload &frameWorkload = workloads[(threadCursor + i) % workloads.size()];
                        drawnCount += (frameWorkload.fbPairCount > 0) ? 1 : 0;
                        shownFbAddress = frameWorkload.drawsShown ? frameWorkload.shownFbAddress : shownFbAddress;
                    }

                    const bool frameRest = lastFrameCut && (shownFbAddress != 0) && (shownFbAddress == lastFrameShownFbAddress) &&
                        (shownFbAddress != frameBeforeShownFbAddress);
                    if (frameRest) {
                        lastFrameDrawnCount += drawnCount;
                    }
                    else {
                        lastFrameDrawnCount = drawnCount;
                        frameBeforeShownFbAddress = lastFrameShownFbAddress;
                        lastFrameShownFbAddress = shownFbAddress;
                    }

                    lastFrameCut = ((frameEnd == GameFrameEnd::Wait) || (frameEnd == GameFrameEnd::HeldLimit) || (frameEnd == GameFrameEnd::QueueFull));
#               endif
                    processCursor = threadCursor;
                    threadCursor = (threadCursor + frameWorkloadCount) % workloads.size();
                }
            }

            if (processCursor >= 0) {
                // The game waits for the prebuilt shaders (RasterShaderCache::startPrebuild()) here, past its first frame.
                // The present queue counts the waits, to leave the burst of presents after one out of its pacing.
                if (ext.rasterShaderCache->waitForPrebuild()) {
                    ext.sharedResources->prebuildWaits++;
                }

                std::unique_lock<std::mutex> threadLock(threadMutex);
                Workload &workload = workloads[processCursor];
                const Workload &lastWorkload = workloads[(processCursor + frameWorkloadCount - 1) % workloads.size()];
                ext.presentQueue->waitForPresentId(workload.presentId);

                if (!threadsRunning) {
                    continue;
                }

                ElapsedTimer workloadTimer;
                workloadProfiler.start();
                threadConfigurationUpdate(lastWorkload.viFbSize, workloadConfig);

                // FIXME: This is a very hacky way to find out if we need to advance the frame if the workload was paused for the first time.
                if (!workload.paused || (!gameFrames[curFrameIndex].workloads.empty() && (gameFrames[curFrameIndex].workloads[0] != (uint32_t)processCursor))) {
                    prevFrameIndex = curFrameIndex;
                    curFrameIndex = (curFrameIndex + 1) % gameFrames.size();
                }

                GameFrame &curFrame = gameFrames[curFrameIndex];
                const GameFrame &prevFrame = gameFrames[prevFrameIndex];
                thread_local std::vector<uint32_t> workloadIndices;
                workloadIndices.clear();
                for (uint32_t w = 0; w < frameWorkloadCount; w++) {
                    workloadIndices.emplace_back(uint32_t((processCursor + w) % workloads.size()));
                }

                curFrame.set(*this, workloadIndices.data(), frameWorkloadCount);

                // Detect the color image to interpolate for this frame: the last one its workloads draw that can be.
                RenderTargetKey interpolationTargetKey;
                uint32_t interpolationTargetWorkload = 0;
                int32_t interpolationTargetFbPairIndex = -1;
                {
                    std::scoped_lock<std::mutex> managerLock(ext.sharedResources->managerMutex);
                    FramebufferManager &fbManager = ext.sharedResources->framebufferManager;
                    std::vector<uint32_t> &colorVector = ext.sharedResources->colorImageAddressVector;
                    std::unordered_set<uint32_t> &colorSet = ext.sharedResources->colorImageAddressSet;
                    colorVector.clear();
                    colorSet.clear();
                    for (int32_t w = int32_t(frameWorkloadCount) - 1; w >= 0; w--)
                    for (int32_t f = int32_t(workloads[workloadIndices[w]].fbPairCount) - 1; f >= 0; f--) {
                        const FramebufferPair &fbPair = workloads[workloadIndices[w]].fbPairs[f];
                        bool interpolationCandidate = fbPair.earlyPresentCandidate();
                        if (fbPair.drawColorRect.isEmpty()) {
                            continue;
                        }

                        const auto &colorImg = fbPair.colorImage;
                        if (colorSet.find(colorImg.address) != colorSet.end()) {
                            continue;
                        }
                        else {
                            if (interpolationCandidate) {
                                colorVector.push_back(colorImg.address);
                            }

                            colorSet.insert(colorImg.address);
                        }

                        if (!interpolationCandidate || !interpolationTargetKey.isEmpty()) {
                            continue;
                        }

                        Framebuffer *interpolationFb = fbManager.find(colorImg.address);
                        if ((interpolationFb != nullptr) && interpolationFb->interpolationEnabled) {
                            interpolationTargetKey.fbType = Framebuffer::Type::Color;
                            interpolationTargetKey.address = fbPair.colorImage.address;
                            interpolationTargetKey.siz = fbPair.colorImage.siz;
                            interpolationTargetKey.width = fbPair.colorImage.width;
                            interpolationTargetWorkload = uint32_t(w);
                            interpolationTargetFbPairIndex = f;
                        }
                    }
                }

                float prevFrameWeight = 0.0f;
                float curFrameWeight = 1.0f;
                float deltaTimeMs = 1.0f / 30.0f;
                const bool requiresFrameMatching = (workloadConfig.targetRate > 0) || workloadConfig.raytracingEnabled;
                bool generateInterpolatedFrames = false;
                bool velocityUploaderUsed = false;
                bool tileInterpolationUsed = false;
                bool lookAtInterpolationUsed = false;
                if (requiresFrameMatching) {
#               if RT64_GAME_FRAME_WORKLOADS
                    // The frame's rate is its last workload's (the first one's pause is the same: a paused workload is a frame
                    // of its own). Named as upstream's lines below name it, which a recomp's own rt64 patch may change.
                    const Workload &workload = lastWorkload;
#               endif
                    matchingProfiler.reset();
                    matchingProfiler.start();
                    curFrame.match(ext.workloadGraphicsWorker, *this, prevFrame, ext.workloadVelocityUploader, velocityUploaderUsed, tileInterpolationUsed, lookAtInterpolationUsed);
                    matchingProfiler.end();
                    matchingProfiler.log();

                    const bool displayRateAboveOriginal = (workload.viOriginalRate > 0) && (workloadConfig.targetRate > workload.viOriginalRate);
                    generateInterpolatedFrames = !workload.paused && displayRateAboveOriginal && !interpolationTargetKey.isEmpty();

                    const bool resetTicks = !generateInterpolatedFrames || (originalRateForTicks != workload.viOriginalRate) || (displayRateForTicks != workloadConfig.targetRate) || !displayRateAboveOriginal;
                    if (resetTicks) {
                        logicalTicks = 0;
                        displayTicks = 0;
                        originalRateForTicks = workload.viOriginalRate;
                        displayRateForTicks = workloadConfig.targetRate;
                    }
                }

                // Estimate amount of frames to render based on how many display frames it'd take to reach the next logical frame.
                uint32_t displayFrames = 1;
                if (generateInterpolatedFrames) {
                    logicalTicks += workloadConfig.targetRate;
                    displayFrames = uint32_t((logicalTicks - displayTicks) / lastWorkload.viOriginalRate);
                    deltaTimeMs = 1.0f / float(workloadConfig.targetRate);

                    if ((displayFrames > 1) && frameReduction) {
                        displayTicks += lastWorkload.viOriginalRate;
                        displayFrames--;
                        frameReduction = false;
                    }

                    assert((logicalTicks > displayTicks) && "Logical ticks must always remain bigger than the display ticks.");
                    assert(((logicalTicks - displayTicks) <= (workloadConfig.targetRate + lastWorkload.viOriginalRate)) && "The gap between logical ticks and display ticks can't be bigger than the target rate.");
                    assert((displayFrames > 0) && "At least one display frame must be generated.");
                }
                else if (lastWorkload.viOriginalRate > 0) {
                    deltaTimeMs = 1.0f / float(lastWorkload.viOriginalRate);
                }

                ext.sharedResources->viOriginalRate = lastWorkload.viOriginalRate;
                
                // Get the current and previous set of frame counters. The other set can be in use by the present queue. Skip if no new present event has arrived before this workload event.
                InterpolatedFrameCounters &prevFrameCounters = ext.sharedResources->interpolatedFrames[ext.sharedResources->interpolatedFramesIndex];
                const bool useDifferentCounters = (lastPresentId != workload.presentId);
                if (useDifferentCounters) {
#               if RT64_GAME_FRAME_WORKLOADS
                    std::scoped_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
#               endif
                    ext.sharedResources->interpolatedFramesIndex = ext.sharedResources->interpolatedFramesIndex ^ 1;
                    lastPresentId = workload.presentId;
                }
                // If the same set of counters is used, we wait until the presentation of its targets is finished so the targets are available to use. Waiting is ignored
                // if the frame counter has never presented anything yet, as it'll only be a valid value if the previous present event actually did something.
                else if (generateInterpolatedFrames) {
                    std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
                    ext.sharedResources->interpolatedCondition.wait(interpolatedLock, [&]() {
                        return (prevFrameCounters.presented == 0) || (prevFrameCounters.presented >= prevFrameCounters.available);
                    });
                }

                InterpolatedFrameCounters &curFrameCounters = ext.sharedResources->interpolatedFrames[ext.sharedResources->interpolatedFramesIndex];
#           if RT64_GAME_FRAME_WORKLOADS
                // Not while a present holds them (InterpolatedFrameCounters::inUse): the frame it shows has ended, so it does.
                std::unique_lock<std::mutex> curInterpolatedLock(ext.sharedResources->interpolatedMutex);
                ext.sharedResources->interpolatedCondition.wait(curInterpolatedLock, [&]() {
                    return !curFrameCounters.inUse || !threadsRunning;
                });
#           endif
                curFrameCounters.skipped = false;
                curFrameCounters.presented = 0;
                curFrameCounters.available = 0;
                curFrameCounters.count = displayFrames;
#           if RT64_GAME_FRAME_WORKLOADS
                curInterpolatedLock.unlock();
#           endif

                // Create as many render targets as required to store the interpolated targets.
                auto &interpolatedTargets = ext.sharedResources->interpolatedColorTargets;
                const bool usingMSAA = (ext.sharedResources->renderTargetManager.multisampling.sampleCount > 1);
                const bool usesHDR = ext.sharedResources->renderTargetManager.usesHDR;
                uint32_t requiredFrames = (usingMSAA && generateInterpolatedFrames) ? displayFrames : (displayFrames - 1);
#           if defined(__EMSCRIPTEN__)
                // At most InterpolatedTargetSlots targets, used in turn: a frame past them waits for the frame drawn into its
                // target to be presented. RT64 keeps a target for every display frame of a game frame and never frees one,
                // so a page held as many as the slowest game frame of the visit took display frames: GoldenEye's targets
                // are 5702x2970 (68 MB) at a 3840x2000 canvas, and ?uncapped (360 Hz) in Dam at ~8 fps kept 42 of them,
                // 2.8 GB, until Firefox's device ran out of memory. At 60 Hz a game frame takes more only below 7.5 fps.
                requiredFrames = std::min(requiredFrames, InterpolatedTargetSlots);
#           endif
                if ((requiredFrames > 0) && (interpolatedTargets.size() < requiredFrames)) {
                    // The present thread indexes this vector under the lock while it shows the previous game frame's
                    // targets, and growing it can move every element.
                    std::scoped_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
                    uint32_t previousSize = uint32_t(interpolatedTargets.size());
                    interpolatedTargets.resize(requiredFrames);
                    for (uint32_t i = previousSize; i < requiredFrames; i++) {
                        interpolatedTargets[i] = std::make_unique<RenderTarget>(interpolationTargetKey.address, Framebuffer::Type::Color, RenderMultisampling(), usesHDR);
                    }
                }
                
                const int64_t originalTimeMicro = (lastWorkload.viOriginalRate > 0) ? (1000000 / lastWorkload.viOriginalRate) : 0;
                const int64_t setupTimeMicro = workloadTimer.elapsedMicroseconds();
                const int64_t adjustedTimeWindowMicro = originalTimeMicro - setupTimeMicro;
                const int64_t maxTimePerFrameMicro = adjustedTimeWindowMicro / displayFrames;
                bool skippedFrames = false;
                bool skipWorkloadNow = false;
                uint32_t targetIndex = 0;
                uint32_t framesRendered = 0;
                auto newerWorkloadQueued = [&]() {
                    std::scoped_lock<std::mutex> cursorLock(cursorMutex);
#               if RT64_GAME_FRAME_WORKLOADS
                    // The next frame's first workloads alone (an offscreen pass) aren't a frame: these frames go on. Not
                    // while the queue is full, though: the game waits for this frame before it queues another workload or
                    // the present that shows this one, so no frame can end and a frame's interpolated frames past
                    // InterpolatedTargetSlots would wait for presents that never come. A frame the queue ended when it
                    // was full held every workload there was, and Mario Party 2's page stopped for good in its opening,
                    // uncapped, with 8 of a frame's 12 drawn.
                    GameFrameEnd nextEnd;
                    return (endedGameFrame(threadCursor, nextEnd) > 0) ||(((writeCursor + 1) % int(workloads.size())) == barrierCursor);
#               else
                    return (writeCursor != threadCursor);
#               endif
                };
                int64_t renderTimeTotalMicro = 0;
                for (uint32_t frame = 0; (frame < displayFrames) && !skipWorkloadNow; frame++) {
                    // Evaluate if this frame should be skipped. Measure the current time and compare it to what frame is estimated should be have been rendered by now.
                    if ((frame > 0) && (originalTimeMicro > 0)) {
                        const int64_t currentTimeMicro = workloadTimer.elapsedMicroseconds() - setupTimeMicro;
                        const int64_t expectedTimeMicro = frame * maxTimePerFrameMicro;
                        const int64_t measuredFrameMicro = renderTimeTotalMicro / framesRendered;
                        if ((currentTimeMicro > expectedTimeMicro) || ((currentTimeMicro + measuredFrameMicro) > adjustedTimeWindowMicro)) {
                            displayTicks += lastWorkload.viOriginalRate;
                            skippedFrames = true;
                            continue;
                        }
                    }

                    RenderTarget *overrideTarget = nullptr;
                    uint32_t overrideModifier = 0;
                    if (generateInterpolatedFrames) {
                        prevFrameWeight = std::clamp((workloadConfig.targetRate + displayTicks - logicalTicks) / float(workloadConfig.targetRate), 0.0f, 1.0f);
                        displayTicks += lastWorkload.viOriginalRate;
                        curFrameWeight = std::clamp((workloadConfig.targetRate + displayTicks - logicalTicks) / float(workloadConfig.targetRate), 0.0f, 1.0f);

                        // Override the render target.
                        if (usingMSAA || (frame > 0)) {
                            // The targets are used in turn when there are fewer than the frames (InterpolatedTargetSlots).
                            const uint32_t targetSlots = uint32_t(interpolatedTargets.size());
                            const uint32_t targetSlot = targetIndex % targetSlots;
                            overrideTarget = interpolatedTargets[targetSlot].get();
                            overrideModifier = (targetSlot + 1);

#                       if defined(__EMSCRIPTEN__)
                            if (targetIndex >= targetSlots) {
                                // Wait until this workload's own frame drawn into the target has been presented, unless a newer
                                // workload is waiting: then the rest of this one's frames are skipped, as they are after a frame.
                                std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
                                while ((curFrameCounters.presented <= (targetIndex - targetSlots)) && !skipWorkloadNow) {
                                    ext.sharedResources->interpolatedCondition.wait_for(interpolatedLock, std::chrono::milliseconds(5));
                                    if (curFrameCounters.presented <= (targetIndex - targetSlots)) {
                                        interpolatedLock.unlock();
                                        skipWorkloadNow = newerWorkloadQueued();
                                        interpolatedLock.lock();
                                    }
                                }

                                if (skipWorkloadNow) {
                                    curFrameCounters.skipped = true;
                                    interpolatedLock.unlock();
                                    displayTicks += lastWorkload.viOriginalRate * (displayFrames - (frame + 1));
                                    ext.sharedResources->interpolatedCondition.notify_all();
                                    continue;
                                }
                            }
#                       endif

                            if (useDifferentCounters && (prevFrameCounters.available > targetSlot)) {
                                // Wait until the target has finished presenting if the alternate frame counter (used by the present queue) is making use of this target.
                                const uint32_t prevTargetIndex = targetSlot + ((prevFrameCounters.available - 1 - targetSlot) / targetSlots) * targetSlots;
                                std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
                                ext.sharedResources->interpolatedCondition.wait(interpolatedLock, [&]() {
                                    frameReduction = frameReduction || (prevFrameCounters.presented <= prevTargetIndex);
#                               if RT64_GAME_FRAME_WORKLOADS
                                    // Only while a present holds them (InterpolatedFrameCounters::inUse).
                                    return (prevFrameCounters.presented > prevTargetIndex) || !prevFrameCounters.inUse;
#                               else
                                    return prevFrameCounters.presented > prevTargetIndex;
#                               endif
                                });
                            }

                            targetIndex++;
                        }
                    }
                    else if (workload.paused) {
                        curFrameWeight = lastWorkload.debuggerRenderer.interpolationWeight;
                        prevFrameWeight = 1.0f - curFrameWeight;
                    }
                    else {
                        prevFrameWeight = 0.0f;
                        curFrameWeight = 1.0f;
                    }

                    const bool uploadExtras = (frame == 0) && workloadConfig.raytracingEnabled;
                    if (uploadExtras) {
                        std::vector<BufferUploader::Upload> extrasUploads;
                        for (uint32_t workloadIndex : workloadIndices) {
                            Workload &frameWorkload = workloads[workloadIndex];
                            extrasUploads.push_back({ frameWorkload.drawData.extraParams.data(), { 0, frameWorkload.drawData.extraParams.size() }, sizeof(interop::ExtraParams), RenderBufferFlag::STORAGE, {}, &frameWorkload.drawBuffers.extraParamsBuffer });
                        }

                        ext.workloadExtrasUploader->submit(ext.workloadGraphicsWorker, extrasUploads);
                    }

                    int64_t renderTimeMicro = workloadTimer.elapsedMicroseconds();
                    threadRenderFrame(curFrame, prevFrame, workloadConfig, lastWorkload.debuggerRenderer, lastWorkload.debuggerCamera, curFrameWeight, prevFrameWeight, deltaTimeMs,
                        interpolationTargetKey, interpolationTargetWorkload, interpolationTargetFbPairIndex, overrideTarget, overrideModifier, velocityUploaderUsed, uploadExtras, tileInterpolationUsed, lookAtInterpolationUsed);

                    // Add total time the frame took to render.
                    renderTimeTotalMicro += workloadTimer.elapsedMicroseconds() - renderTimeMicro;

                    // After one frame is rendered, we indicate the workload has been processed so the present thread can start presenting frames as soon as it can.
                    if (frame == 0) {
                        threadAdvanceWorkloadId(lastWorkload.workloadId);
                    }

                    // For every additional frame, we increase the frames available and notify the present queue.
                    if (generateInterpolatedFrames && (usingMSAA || (frame > 0))) {
                        skipWorkloadNow = ((frame + 1) < displayFrames) && newerWorkloadQueued();

                        {
                            std::scoped_lock<std::mutex> managerLock(ext.sharedResources->interpolatedMutex);
                            curFrameCounters.skipped = skipWorkloadNow;
                            curFrameCounters.available++;
                        }

                        // Add the amount of display ticks that correspond to the remaining frames.
                        if (skipWorkloadNow) {
                            displayTicks += lastWorkload.viOriginalRate * (displayFrames - (frame + 1));
                        }

                        ext.sharedResources->interpolatedCondition.notify_all();
                    }

                    framesRendered++;
                }

                // Set the skipped parameter on the frame counter if the workload wasn't skipped but some of its frames were.
                if (skippedFrames && !skipWorkloadNow) {
                    {
                        std::scoped_lock<std::mutex> managerLock(ext.sharedResources->interpolatedMutex);
                        curFrameCounters.skipped = true;
                    }

                    ext.sharedResources->interpolatedCondition.notify_all();
                }

                threadConfigurationValidate();

#           if defined(__EMSCRIPTEN__)
                // present-stats: every 5 s or so, how many game frames a second came and how many frames were drawn for them.
                // A game slower than the rate it reports (viOriginalRate) is its own frames falling behind, on the CPU, where
                // presents short of the display's rate can be either: Rush 2049's race on a phone reported 30 and came at
                // 19-25, while RT64 drew 46 frames a second for them. The clock is read every 16th frame: on a phone each
                // read is a trip out of wasm that costs.
                if (ext.presentQueue->presentStats) {
                    if ((statsGameFrames == 0) && (statsFramesDrawn == 0)) {
                        statsStart = Timer::current();
                    }

                    statsGameFrames++;
                    statsFramesDrawn += framesRendered;
                    if ((statsGameFrames % 16) == 0) {
                        const double seconds = Timer::deltaMicroseconds(statsStart, Timer::current()) / 1000000.0;
                        if (seconds >= 5.0) {
                            printf("RT64: %.1f game frames a second (the game reports %u), %.1f frames drawn for them\n", statsGameFrames / seconds,
                                lastWorkload.viOriginalRate, statsFramesDrawn / seconds);
                            statsGameFrames = 0;
                            statsFramesDrawn = 0;
                        }
                    }
                }
#           endif

                // The frame is the next one's frame before: the writer stops short of its first workload.
                if (!workload.paused) {
#               if RT64_GAME_FRAME_WORKLOADS
                    {
                        std::scoped_lock<std::mutex> cursorLock(cursorMutex);
                        barrierCursor = processCursor;
                    }

                    cursorCondition.notify_all();
#               else
                    threadAdvanceBarrier();
#               endif
                }

                processCursor = -1;
                workloadProfiler.end();
                workloadProfiler.log();
                workloadProfiler.reset();
            }
        }
    }
    
    void WorkloadQueue::idleThreadLoop() {
        // Beware traveler as you enter the zone of dirty driver hacks. Given N64 games are not exactly a demanding thing to render
        // nowadays for modern GPUs and due to how the plugin's cooperative multiqueue system works, it's sometimes just not possible
        // to keep the GPU busy at all times. It is often the case that the GPU might've already rendered all the frames it needed to
        // generate before the screen update event from the emulator even arrives on time.
        // 
        // Under this situation, some drivers are a bit too trigger-happy to downclock the GPU and lower the power consumption, eventually
        // resulting in very low power states that cause unwanted frametime spikes that can no longer reach the target framerate. This
        // results in visible judder during gameplay.
        //
        // This thread will take care of sending some GPU work that does nothing useful while the GPU is not actually busy generating
        // new frames. The waiting interval is close to the minimum resolution the OS provides and big enough to not cause any significant
        // delays or unwanted power consumption: it's just enough to keep the driver from downclocking to a power state level that is
        // usually intended for 2D work or video playback.
        //
        // This workaround is not required if the driver is configured to be at the "Max Performance" power state.

        Thread::setCurrentThreadName("RT64 Idle");

        const ShaderRecord &idle = ext.shaderLibrary->idle;
        RenderCommandList *commandList = ext.workloadGraphicsWorker->commandList.get();
        while (threadsRunning) {
            {
                std::unique_lock<std::mutex> idleLock(idleMutex);
                idleCondition.wait(idleLock, [&]() {
                    return idleActive || !threadsRunning;
                });
            }

            if (threadsRunning) {
                if (workerMutex.try_lock()) {
                    commandList->begin();
                    commandList->setPipeline(idle.pipeline.get());
                    commandList->setComputePipelineLayout(idle.pipelineLayout.get());
                    commandList->dispatch(1, 1, 1);
                    commandList->end();
                    ext.workloadGraphicsWorker->execute();
                    ext.workloadGraphicsWorker->wait();
                    workerMutex.unlock();
                }
                
                Thread::sleepMilliseconds(1);
            }
        }
    }
};