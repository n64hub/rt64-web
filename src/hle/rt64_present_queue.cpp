//
// RT64
//

#include "rt64_present_queue.h"

#include "common/rt64_thread.h"
#include "rhi/rt64_render_hooks.h"

#if defined(__EMSCRIPTEN__)
#   include <emscripten.h>
#endif

#include "rt64_workload_queue.h"

namespace RT64 {
    // PresentQueue

    PresentQueue::PresentQueue() {
        reset();
    }

    PresentQueue::~PresentQueue() {
        presentThreadRunning = false;
        cursorCondition.notify_all();

        if (presentThread != nullptr) {
            presentThread->join();
            delete presentThread;
        }

        presentIdCondition.notify_all();
    }

    void PresentQueue::reset() {
        threadCursor = 0;
        writeCursor = 0;
        barrierCursor = 0;
        presentId = 0;
    }

    void PresentQueue::advanceToNextPresent() {
        // For present-stats, when the present was queued: before the stall below, which waits only while the present
        // thread is the whole queue behind.
        if (presentStats) {
            presentQueued[writeCursor] = Timer::current();
        }

        int nextWriteCursor = (writeCursor + 1) % presents.size();

        // Stall the thread until the barrier is lifted if we're trying to write on a present being used by the GPU.
        bool waitForBarrier;
        do {
            const std::scoped_lock lock(cursorMutex);
            waitForBarrier = (nextWriteCursor == barrierCursor);
#if RT64_GAME_FRAME_WORKLOADS
            writerWaiting = waitForBarrier;
#endif
        } while (waitForBarrier);

        // Modify the cursor and notify anything waiting on the queue.
        {
            const std::scoped_lock lock(cursorMutex);
            writeCursor = nextWriteCursor;
        }

        cursorCondition.notify_all();
    }

    void PresentQueue::repeatLastPresent() {
        {
            const std::scoped_lock lock(cursorMutex);
            threadCursor = previousWriteCursor();
        }

        cursorCondition.notify_all();
    }

    uint32_t PresentQueue::previousWriteCursor() const {
        if (writeCursor > 0) {
            return writeCursor - 1;
        }
        else {
            return uint32_t(presents.size()) - 1;
        }
    }

    void PresentQueue::waitForIdle() {
        std::unique_lock<std::mutex> threadLock(threadMutex);
    }

    void PresentQueue::waitForPresentId(uint64_t waitId) {
        std::unique_lock<std::mutex> presentLock(presentIdMutex);
        presentIdCondition.wait(presentLock, [&]() {
            return (waitId <= presentId) || !presentThreadRunning;
        });
    }

    void PresentQueue::setup(const External &ext) {
        this->ext = ext;

        viRenderer = std::make_unique<VIRenderer>();

#   if defined(__EMSCRIPTEN__)
        // A page opened with ?uncapped measures how many presents a second it can make: plume reports the display's rate
        // as the query's (360), the frames are interpolated to it, and none are paced (pace()), so that what the GPU
        // and the browser can take is what is presented. The interpolated frames take turns in 8 color targets of the
        // game's framebuffer at the output's scale (WorkloadQueue's InterpolatedTargetSlots), 0.25-0.55 GB at 3840x2000.
        uncapped = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).has('uncapped') ? 1 : 0; }) != 0;
        presentStats = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).has('present-stats') ? 1 : 0; }) != 0;
#   endif

        presentThreadRunning = true;
        presentThread = new std::thread(&PresentQueue::threadLoop, this);
    }

    void PresentQueue::threadPresent(const Present &present, bool &swapChainValid) {
        FramebufferManager &fbManager = ext.sharedResources->framebufferManager;
        RenderTargetManager &targetManager = ext.sharedResources->renderTargetManager;
        const bool usingMSAA = (targetManager.multisampling.sampleCount > 1);
        hlslpp::float2 resolutionScale;
        EnhancementConfiguration::Presentation::Mode presentationMode;
        bool removeBlackBorders;
        UserConfiguration::RefreshRate refreshRate;
        UserConfiguration::Filtering filtering;
        uint32_t viOriginalRate;
        uint32_t targetRate;
        {
            std::scoped_lock<std::mutex> configurationLock(ext.sharedResources->configurationMutex);
            resolutionScale = ext.sharedResources->resolutionScale;
            presentationMode = ext.sharedResources->enhancementConfig.presentation.mode;
            removeBlackBorders = ext.sharedResources->enhancementConfig.presentation.removeBlackBorders;
            refreshRate = ext.sharedResources->userConfig.refreshRate;
            filtering = ext.sharedResources->userConfig.filtering;
            viOriginalRate = ext.sharedResources->viOriginalRate;
            targetRate = ext.sharedResources->targetRate;
        }

        // Presents are paced to a lower rate while the GPU can't keep up (pace()).
        const uint32_t pacedRate = ext.sharedResources->pacedRate;
        if ((pacedRate > 0) && (pacedRate < targetRate)) {
            targetRate = pacedRate;
        }

        RenderTarget *colorTarget = nullptr;
        int32_t framesToPresent = 1;
        bool lockedWorkloadMutex = false;
        InterpolatedFrameCounters &frameCounters = ext.sharedResources->interpolatedFrames[ext.sharedResources->interpolatedFramesIndex];

        // TODO: There's a possible race condition interactions that can happen while the workload
        // queue is rendering extra frames and the present event is processed while it's generating
        // interpolated frames. When the framebuffer manager or the render target manager maps are
        // modified while the present queue is retrieving the framebuffer or the target. These can
        // likely be solved by locking the access to the managers during modification.
        
        // Perform any external write operations indicated by the event.
        if (!present.fbOperations.empty()) {
            const std::scoped_lock lock(screenFbChangePoolMutex);
            {
                RenderWorkerExecution workerExecution(ext.presentGraphicsWorker);
                fbManager.performOperations(ext.presentGraphicsWorker, &screenFbChangePool, nullptr, ext.shaderLibrary, nullptr,
                    present.fbOperations, targetManager, resolutionScale, 0, 0, nullptr);
            }
        }

        // Present the VI specified by the event.
        // Attempt to find the matching framebuffer for the VI based on the origin address.
        // If that fails, we look at the shared storage.
        if (present.screenVI.visible()) {
            Framebuffer *viFb = nullptr;
            if (!viewRDRAM) {
                viFb = fbManager.find(present.screenVI.fbAddress());
            }

            Framebuffer *presentFb = viFb;
            
            // Show the framebuffer the debugger has requested instead.
            if (present.debuggerFramebuffer.view) {
                Framebuffer *candidateFb = fbManager.find(present.debuggerFramebuffer.address);
                if (candidateFb != nullptr) {
                    presentFb = candidateFb;
                }
            }
            
            if ((presentFb != nullptr) && (viFb != nullptr)) {
                for (uint32_t colorAddress : ext.sharedResources->colorImageAddressVector) {
                    Framebuffer *colorFb = fbManager.find(colorAddress);
                    if (colorFb == nullptr) {
                        continue;
                    }

                    // Always default to interpolation being disabled for all modified framebuffers.
                    colorFb->interpolationEnabled = false;
                    
                    // When the skip buffering option is on, we check the video history to find if any of the framebuffers that
                    // were drawn in this frame have been previously used for presentation. This is ignored when the debugger
                    // has forced viewing a particular framebuffer.
                    if (!present.debuggerFramebuffer.view && (presentationMode == EnhancementConfiguration::Presentation::Mode::SkipBuffering)) {
                        for (size_t h = 0; h < viHistory.history.size(); h++) {
                            const VIHistory::Present &entry = viHistory.history[h];
                            if ((colorFb->addressStart == entry.vi.fbAddress()) && (colorFb->width == entry.fbWidth) && (colorFb->siz == entry.vi.fbSiz()) && entry.vi.compatibleWith(present.screenVI)) {
                                presentFb = colorFb;
                                break;
                            }
                        }
                    }

                    // Present early (or games that behave like it) will make it so that the presented image is a color image
                    // that the workload modified. We run a basic check to see if that holds true to indicate it was presented
                    // so interpolation is possible.
                    if (colorFb == presentFb) {
                        presentFb->interpolationEnabled = true;
                        break;
                    }
                }

                if (presentFb->interpolationEnabled) {
                    framesToPresent = frameCounters.count;
                }
                else {
                    lockedWorkloadMutex = true;
                    ext.sharedResources->workloadMutex.lock();
                }

                RenderTargetKey colorTargetKey(presentFb->addressStart, presentFb->width, presentFb->siz, Framebuffer::Type::Color);
                colorTarget = &targetManager.get(colorTargetKey, true);
                if (!colorTarget->isEmpty()) {
                    // If a depth framebuffer is about to be shown, convert it to color.
                    if (presentFb->isLastWriteDifferent(Framebuffer::Type::Color)) {
                        RenderTargetKey otherColorTargetKey(presentFb->addressStart, presentFb->width, presentFb->siz, presentFb->lastWriteType);
                        RenderTarget &otherColorTarget = targetManager.get(otherColorTargetKey, true);
                        if (!otherColorTarget.isEmpty()) {
                            const FixedRect &r = presentFb->lastWriteRect;
                            RenderWorkerExecution workerExecution(ext.presentGraphicsWorker);
                            colorTarget->copyFromTarget(ext.presentGraphicsWorker, &otherColorTarget, r.left(false), r.top(false), r.width(false, true), r.height(false, true), ext.shaderLibrary);
                        }
                    }
                }
                else {
                    colorTarget = nullptr;
                }

                if (!present.paused && (viHistory.top().vi != present.screenVI)) {
                    viHistory.pushVI(present.screenVI, viFb->width);
                }
            }
            else {
                uint32_t fbAddress = present.screenVI.fbAddress();

                // Use a scratch framebuffer to upload the RAM to the render target.
                hlslpp::uint2 fbSize = present.screenVI.fbSize();
                scratchFb.addressStart = fbAddress;
                scratchFb.width = fbSize.x;
                scratchFb.height = fbSize.y;
                scratchFb.siz = present.screenVI.fbSiz();

                lockedWorkloadMutex = true;
                ext.sharedResources->workloadMutex.lock();

                RenderTargetKey colorTargetKey(fbAddress, scratchFb.width, scratchFb.siz, Framebuffer::Type::Color);
                colorTarget = &targetManager.get(colorTargetKey, true);
                colorTarget->resize(ext.presentGraphicsWorker, scratchFb.width, scratchFb.height);
                colorTarget->resolutionScale = { 1.0f, 1.0f };
                colorTarget->downsampleMultiplier = 1;

                scratchFb.nativeTarget.resetBufferHistory();

                {
                    RenderWorkerExecution workerExecution(ext.presentGraphicsWorker);
                    colorTarget->clearColorTarget(ext.presentGraphicsWorker);
                    FramebufferChange *colorFbChange = scratchFb.readChangeFromBytes(ext.presentGraphicsWorker, scratchFbChangePool, Framebuffer::Type::Color,
                        G_IM_FMT_RGBA, present.storage.data(), 0, scratchFb.height, ext.shaderLibrary);

                    if (colorFbChange != nullptr) {
                        colorTarget->copyFromChanges(ext.presentGraphicsWorker, *colorFbChange, scratchFb.width, scratchFb.height, 0, ext.shaderLibrary);
                    }
                }

                scratchFbChangePool.reset();

                if (!present.paused && (viHistory.top().vi != present.screenVI)) {
                    viHistory.pushVI(present.screenVI, fbSize.x);
                }
            }
        }

        // Create the framebuffers if necessary.
        if (swapChainFramebuffers.empty()) {
            uint32_t textureCount = ext.swapChain->getTextureCount();
            swapChainFramebuffers.resize(textureCount);
            for (uint32_t i = 0; i < textureCount; i++) {
                const RenderTexture *swapChainTexture = ext.swapChain->getTexture(i);
                swapChainFramebuffers[i] = ext.device->createFramebuffer(RenderFramebufferDesc(&swapChainTexture, 1));
            }
        }
        
        for (int32_t i = 0; i < framesToPresent; i++) {
            uint32_t frameCountersNextPresented = 0;
            if ((framesToPresent > 1) && (usingMSAA || (i > 0))) {
                // Stall until the interpolated color target is available.
                const uint32_t targetIndex = usingMSAA ? i : (i - 1);
                PhaseTimer phaseTimer(this, PhaseInterpolated);
                std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
                ext.sharedResources->interpolatedCondition.wait(interpolatedLock, [&]() {
                    return (frameCounters.available > targetIndex) || ((frameCounters.available == targetIndex) && frameCounters.skipped);
                });

                // Do not present any more frames after this one after reaching the last available frame if the workload was skipped.
                if ((frameCounters.available == targetIndex) && frameCounters.skipped) {
                    framesToPresent = std::min(int(frameCounters.available), i + 1);
                    frameCountersNextPresented = frameCounters.count;
                }
                else {
                    frameCountersNextPresented = frameCounters.presented + 1;
                }

                if (i < framesToPresent) {
                    uint32_t targetIndex = usingMSAA ? i : (i - 1);
                    const auto &interpolatedTargets = ext.sharedResources->interpolatedColorTargets;
                    colorTarget = interpolatedTargets[targetIndex % interpolatedTargets.size()].get();
                }
                else {
                    colorTarget = nullptr;
                }
            }
            else if (framesToPresent == 1) {
                frameCountersNextPresented = frameCounters.count;
            }

            uint32_t swapChainIndex = 0;
            const bool presentFrame = (i < framesToPresent) && swapChainValid;
            std::optional<PhaseTimer> drawTimer;
            if (presentFrame) {
                drawTimer.emplace(this, PhaseDraw);
                swapChainValid = ext.swapChain->acquireTexture(acquiredSemaphore.get(), &swapChainIndex);
            }

            if (presentFrame && swapChainValid) {
                // Draw the framebuffer with the VI renderer.
                RenderTexture *swapChainTexture = ext.swapChain->getTexture(swapChainIndex);
                RenderFramebuffer *swapChainFramebuffer = swapChainFramebuffers[swapChainIndex].get();
                RenderCommandList *commandList = ext.presentGraphicsWorker->commandList.get();
                commandList->begin();
                commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::COLOR_WRITE));
                
                VIRenderer::RenderParams renderParams;
                if (colorTarget != nullptr) {
                    renderParams.device = ext.device;
                    renderParams.commandList = commandList;
                    renderParams.swapChain = ext.swapChain;
                    renderParams.shaderLibrary = ext.shaderLibrary;
                    renderParams.textureFormat = colorTarget->format;
                    renderParams.resolutionScale = colorTarget->resolutionScale;
                    renderParams.downsamplingScale = 1;
                    renderParams.filtering = filtering;
                    renderParams.vi = &present.screenVI;
                    renderParams.removeBlackBorders = removeBlackBorders;

                    const bool useDownsampling = (colorTarget->downsampleMultiplier > 1);
                    if (useDownsampling) {
                        colorTarget->downsampleTarget(ext.presentGraphicsWorker, ext.shaderLibrary);
                        renderParams.texture = colorTarget->downsampledTexture.get();
                        renderParams.textureWidth = colorTarget->width / colorTarget->downsampleMultiplier;
                        renderParams.textureHeight = colorTarget->height / colorTarget->downsampleMultiplier;
                        renderParams.downsamplingScale = colorTarget->downsampleMultiplier;
                    }
                    else {
                        colorTarget->resolveTarget(ext.presentGraphicsWorker, ext.shaderLibrary);
                        renderParams.texture = colorTarget->getResolvedTexture();
                        renderParams.textureWidth = colorTarget->width;
                        renderParams.textureHeight = colorTarget->height;
                    }
                }
                
                commandList->setFramebuffer(swapChainFramebuffer);
                commandList->clearColor();

                if (renderParams.texture != nullptr) {
                    commandList->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(renderParams.texture, RenderTextureLayout::SHADER_READ));
                    viRenderer->render(renderParams);
                }

                RenderHookDraw *drawHook = GetRenderHookDraw();
                if (drawHook != nullptr) {
                    drawHook(commandList, swapChainFramebuffer);
                }

                {
                    const std::scoped_lock lock(inspectorMutex);
                    if (inspector != nullptr) {
                        inspector->draw(commandList);
                    }
                    
                    commandList->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::PRESENT));
                    commandList->end();
                    const RenderCommandList *commandList = ext.presentGraphicsWorker->commandList.get();
                    RenderCommandSemaphore *waitSemaphore = acquiredSemaphore.get();
                    RenderCommandSemaphore *signalSemaphore = drawSemaphores[swapChainIndex].get();
                    ext.presentGraphicsWorker->commandQueue->executeCommandLists(&commandList, 1, &waitSemaphore, 1, &signalSemaphore, 1, ext.presentGraphicsWorker->commandFence.get());
                    ext.presentGraphicsWorker->wait();
                }
            }

            drawTimer.reset();
            if (lockedWorkloadMutex) {
                ext.sharedResources->workloadMutex.unlock();
                lockedWorkloadMutex = false;
            }
            
            if (frameCountersNextPresented > 0) {
                {
                    std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
                    frameCounters.presented = frameCountersNextPresented;
                }

                ext.sharedResources->interpolatedCondition.notify_all();
            }

            // As soon as we're done with the first render target, we notify the workload queue it can proceed.
            if (i == 0) {
                notifyPresentId(present);
            }

            if (presentFrame && swapChainValid) {
                // Wait until the approximate time the next present should be at the current intended rate.
                if ((presentTimestamp != Timestamp()) && (targetRate > 0) && (targetRate > viOriginalRate)) {
#               if defined(__EMSCRIPTEN__)
                    // On a fixed timeline: each present's slot is one period after the last one's, not after the moment
                    // it went out, so the time spent past a slot (the sleep waking on the browser's timer, wait()'s trip
                    // to the page's thread) isn't added to every period. In Chrome the pages of games below the
                    // display's rate presented every 18 ms, 55.6 a second, where those at its rate, which never sleep
                    // here, presented 60. A present a whole period past its slot or more starts the slots from itself.
                    // Where the swap chain learns of the display's frames as they come (WebGPU), it sleeps: until a
                    // frame where timed waits end late, else until the slot, woken by the frames and spun from the last
                    // (plume-web e16e003: this thread's 100 us sleeps in Chrome now and then took a tick).
                    const auto presentPeriod = std::chrono::nanoseconds(1'000'000'000 / targetRate);
                    if ((presentSlot == Timestamp()) || ((presentTimestamp - presentSlot) >= presentPeriod)) {
                        presentSlot = presentTimestamp;
                    }

                    presentSlot += presentPeriod;
                    const bool slotAhead = presentStats && (presentSlot > Timer::current());
                    {
                        PhaseTimer phaseTimer(this, PhaseSleep);
                        if (!ext.swapChain->sleepUntil(std::chrono::steady_clock::now() + (presentSlot - Timer::current()))) {
                            Timer::preciseSleepUntil(presentSlot);
                        }
                    }

                    // Only a sleep that had a slot to wait for woke past it: one already past (after a stall) didn't sleep.
                    if (slotAhead) {
                        busyLateMicro += std::max(Timer::deltaMicroseconds(presentSlot, Timer::current()), int64_t(0));
                    }
#               else
                    Timer::preciseSleepUntil(presentTimestamp + std::chrono::nanoseconds(1'000'000'000 / targetRate));
#               endif
                }
#           if defined(__EMSCRIPTEN__)
                else {
                    presentSlot = Timestamp();
                }
#           endif

                if (presentWaitEnabled) {
                    PhaseTimer phaseTimer(this, PhaseWait);
                    ext.swapChain->wait();
                }

                RenderCommandSemaphore *waitSemaphore = drawSemaphores[swapChainIndex].get();
                presentTimestamp = Timer::current();
                PhaseTimer phaseTimer(this, PhasePresent);
                swapChainValid = ext.swapChain->present(swapChainIndex, &waitSemaphore, 1);
                presentProfiler.logAndRestart();
            }
        }
    }

    void PresentQueue::skipInterpolation() {
        {
            std::unique_lock<std::mutex> interpolatedLock(ext.sharedResources->interpolatedMutex);
            InterpolatedFrameCounters &frameCounters = ext.sharedResources->interpolatedFrames[ext.sharedResources->interpolatedFramesIndex];
            frameCounters.presented = frameCounters.count;
        }

        ext.sharedResources->interpolatedCondition.notify_all();
    }

    // Frames interpolated to the display's rate are drawn for evenly spaced moments, so motion looks even only if each
    // is presented. When the GPU can't keep up, a game frame whose present is still waiting behind the next one is
    // dropped whole, and the rest get one, two or three presents: some frames shown three times, some never. That
    // happens in Chrome and Firefox alike on a laptop's integrated graphics. So while frames are being dropped (one
    // present in ten or more over 2 s, 4 at least), each game frame gets fewer presents, a number that divides the
    // display's rate evenly (a 30 fps game on a 60 Hz display: 1, at 30 a second), and a while later it tries the
    // number above again: after 10 s, twice as long each time it falls straight back (2 min at most). The number is
    // kept, not the rate, since a game's own rate moves (Banjo-Kazooie's between 20 and 30). A present dropped now and
    // then is not the GPU falling behind: a pipeline built behind the game (Chrome) drops one every second or two, and
    // counting those (2 in a second) held GoldenEye's cast credits, 8-12 game frames a second, at one present a frame
    // for 10-30 s. Nor is a burst of a few: presents stop while the game waits for prebuilt shaders, come through at once
    // after, and the GPU is busy for a moment with what it was given, so drops are counted over a whole 2 s of presents,
    // never fewer: judged on the first few dozen after a wait, or after a try of the number above (which starts the count
    // again), 4 dropped were enough, and Donkey Kong 64 in Firefox went to 30 presents a second right after its boot's
    // wait and stayed there for 70 s, each try falling straight back. And the count starts again a second after any such
    // wait ends: a wait shorter than 2 s left presents from before it in the count beside the burst after it, and
    // Banjo-Kazooie in Chrome went to 30 a second 0.4 s after its 1.5 s wait, for 10 s. A game at about the display's
    // rate (a launcher's 60 on a 59-61 Hz display) has nothing to be paced to, and its drops while a page loads aren't
    // counted either. Nor is a page opened with ?uncapped paced, which drops presents by design. Nor are presents counted
    // while the page is hidden, when none is presented (below), or for a second after, as after a wait: a hidden page's
    // presents dropped while the browser held its present callbacks (plume's isHidden()), and Mario Kart 64's page on a
    // 240 Hz display went from 114 presents a second to 60 while hidden, each try of the number above falling straight
    // back and doubling the time to the next, so it stayed at 60 for 80 s after the tab came back.
    // A page opened with ?present-stats prints where the present thread's time went before each present it drops
    // (reportDroppedPresent()).
    void PresentQueue::pace(bool presentDropped) {
        const uint32_t targetRate = ext.sharedResources->targetRate;
        const uint32_t originalRate = ext.sharedResources->viOriginalRate;
        auto setPacedRate = [&](uint32_t pacedRate) {
            if ((pacedRate == 0) != (ext.sharedResources->pacedRate == 0)) {
                const char *what = (pacedRate > 0) ? "paced to the game's frames while the GPU falls behind" : "back at the display's rate";
                printf("RT64: presents %s (the game at %u frames a second)\n", what, originalRate);
            }

            ext.sharedResources->pacedRate = pacedRate;
        };

        // Whether a number of presents a game frame is fewer than the display's rate gives it (by more than 5%), and
        // whether it is spaced evenly on the display: the display's rate a whole multiple of the presents' (within 5%,
        // for a 59 Hz display), or one a frame.
        auto isFewer = [&](uint32_t multiple) {
            return (multiple * originalRate * 20) < (targetRate * 19);
        };

        if (uncapped || (targetRate == 0) || (originalRate == 0) || !isFewer(1)) {
            pacingMultiple = 0;
            pacingPresents.clear();
            pacingDropped = 0;
            setPacedRate(0);
            return;
        }

        auto isEven = [&](uint32_t multiple) {
            const uint32_t rate = multiple * originalRate;
            const uint32_t whole = (targetRate + rate / 2) / rate;
            return (multiple == 1) || ((whole > 0) && (uint32_t(std::abs(int(targetRate) - int(whole * rate))) * 20 <= targetRate));
        };

        // The most presents a game frame can get that are fewer and even, at most a number, or one.
        auto pacedMultipleAtMost = [&](uint32_t multiple) {
            while ((multiple > 1) && (!isFewer(multiple) || !isEven(multiple))) {
                multiple--;
            }

            return std::max(multiple, 1U);
        };

        const Timestamp now = Timer::current();
        const int64_t minRetryMicro = 10'000'000;
        const int64_t maxRetryMicro = 120'000'000;
        const int64_t fallBackMicro = 3'000'000;
        if (pacingRetryMicro == 0) {
            pacingRetryMicro = minRetryMicro;
            pacingChanged = now;
        }

        const uint32_t prebuildWaits = ext.sharedResources->prebuildWaits;
        if (prebuildWaits != pacingPrebuildWaits) {
            pacingPrebuildWaits = prebuildWaits;
            pacingWaited = now;
        }

        if (ext.swapChain->isHidden()) {
            pacingWaited = now;
        }

        if ((pacingWaited != Timestamp()) && (Timer::deltaMicroseconds(pacingWaited, now) < 1'000'000)) {
            pacingPresents.clear();
            pacingDropped = 0;
            return;
        }

        pacingPresents.emplace_back(now, presentDropped);
        pacingDropped += presentDropped ? 1 : 0;
        while (Timer::deltaMicroseconds(pacingPresents.front().first, now) > 2'000'000) {
            pacingDropped -= pacingPresents.front().second ? 1 : 0;
            pacingPresents.pop_front();
        }

        const bool wholeWindow = Timer::deltaMicroseconds(pacingPresents.front().first, now) >= 1'900'000;
        const bool fallingBehind = wholeWindow && (pacingDropped >= 4) && ((pacingDropped * 10) >= pacingPresents.size());
        const int64_t sinceChange = Timer::deltaMicroseconds(pacingChanged, now);
        if (fallingBehind && (pacingMultiple != 1)) {
            pacingMultiple = pacedMultipleAtMost((pacingMultiple > 0) ? (pacingMultiple - 1) : (targetRate / originalRate));

            if ((pacingRaised != Timestamp()) && (Timer::deltaMicroseconds(pacingRaised, now) < fallBackMicro)) {
                pacingRetryMicro = std::min(pacingRetryMicro * 2, maxRetryMicro);
            }

            pacingPresents.clear();
            pacingDropped = 0;
            pacingChanged = now;
        }
        else if ((pacingMultiple > 0) && (sinceChange >= pacingRetryMicro)) {
            do {
                pacingMultiple++;
            } while (isFewer(pacingMultiple) && !isEven(pacingMultiple));

            if (!isFewer(pacingMultiple)) {
                pacingMultiple = 0;
            }

            pacingPresents.clear();
            pacingDropped = 0;
            pacingChanged = now;
            pacingRaised = now;
        }
        else if ((pacingMultiple == 0) && (sinceChange >= maxRetryMicro)) {
            pacingRetryMicro = minRetryMicro;
        }

        // The game's rate may have moved since the number was picked, so it is the even number at most that large.
        setPacedRate((pacingMultiple > 0) ? (pacedMultipleAtMost(pacingMultiple) * originalRate) : 0);
    }

    PresentQueue::PhaseTimer::PhaseTimer(PresentQueue *queue, PresentPhase phase) : queue(queue), phase(phase) {
        if (queue->presentStats) {
            start = Timer::current();
        }
    }

    PresentQueue::PhaseTimer::~PhaseTimer() {
        if (queue->presentStats) {
            queue->busyPhaseMicro[phase] += Timer::deltaMicroseconds(start, Timer::current());
        }
    }

    void PresentQueue::resetBusyFrames() {
        busyPhaseMicro.fill(0);
        busyLateMicro = 0;
        busyFrames = 0;
        busyStart = Timer::current();
    }

    // With present-stats, a line for each present dropped because the next was already queued behind it (what pace()
    // counts): how long it waited, and where the present thread's time went in the game frames since it last waited for
    // a present or dropped one, as each frame's share: waiting for its workload, for the next interpolated frame, drawing
    // and submitting each present, sleeping until each one's slot (and how far past the slots it woke), wait() (for
    // fewer presents in flight, and the trips to the device thread it makes to ask), the present() calls, and the rest.
    // The thread fell a game frame behind in those frames, so what grew in them is what dropped the present.
    void PresentQueue::reportDroppedPresent(int droppedCursor, int nextCursor) {
        const Timestamp now = Timer::current();
        const double waitedMs = Timer::deltaMicroseconds(presentQueued[droppedCursor], now) / 1000.0;
        const double nextMs = Timer::deltaMicroseconds(presentQueued[droppedCursor], presentQueued[nextCursor]) / 1000.0;
        if (busyFrames == 0) {
            printf("RT64: present dropped (it waited %.1f ms, the next came %.1f ms after it)\n", waitedMs, nextMs);
        }
        else {
            const double frameMs = Timer::deltaMicroseconds(busyStart, now) / 1000.0 / busyFrames;
            auto phaseMs = [&](PresentPhase phase) {
                return busyPhaseMicro[phase] / 1000.0 / busyFrames;
            };

            double restMs = frameMs;
            for (uint32_t p = 0; p < PhaseCount; p++) {
                restMs -= phaseMs(PresentPhase(p));
            }

            printf("RT64: present dropped (it waited %.1f ms, the next came %.1f ms after it); the %u game frames before it "
                "since the present thread last waited took %.1f ms each: workload %.1f, interpolated %.1f, draw %.1f, "
                "sleep %.1f (%.1f past the slots), wait() %.1f, present() %.1f, the rest %.1f\n",
                waitedMs, nextMs, busyFrames, frameMs, phaseMs(PhaseWorkload), phaseMs(PhaseInterpolated), phaseMs(PhaseDraw),
                phaseMs(PhaseSleep), busyLateMicro / 1000.0 / busyFrames, phaseMs(PhaseWait), phaseMs(PhasePresent), restMs);
        }

        resetBusyFrames();
    }

    void PresentQueue::notifyPresentId(const Present &present) {
        {
            std::scoped_lock<std::mutex> cursorLock(presentIdMutex);
            presentId = present.presentId;
        }

        presentIdCondition.notify_all();
    }
    
    void PresentQueue::threadAdvanceBarrier() {
        std::scoped_lock<std::mutex> cursorLock(cursorMutex);
        barrierCursor = (barrierCursor + 1) % presents.size();
    }

    void PresentQueue::threadLoop() {
        Thread::setCurrentThreadName("RT64 Present");

        // Create the semaphore the acquire method will use.
        acquiredSemaphore = ext.device->createCommandSemaphore();

        // Create as many semaphores to signal as textures there are.
        while (drawSemaphores.size() < ext.swapChain->getTextureCount()) {
            drawSemaphores.emplace_back(ext.device->createCommandSemaphore());
        }

        // Since the swap chain might not need a resize right away, detect present wait.
        presentWaitEnabled = ext.device->getCapabilities().presentWait;

        int processCursor = -1;
        bool skipPresent = false;
        bool newerQueued = false;
        uint32_t displayTimingRate = UINT32_MAX;
        const bool displayTiming = ext.device->getCapabilities().displayTiming;
        bool swapChainValid = !ext.swapChain->needsResize();
        while (presentThreadRunning) {
            bool waited = false;
            {
                std::unique_lock<std::mutex> cursorLock(cursorMutex);
                waited = (writeCursor == threadCursor);
                cursorCondition.wait(cursorLock, [&]() {
                    return (writeCursor != threadCursor) || !presentThreadRunning;
                });

                if (presentThreadRunning) {
                    processCursor = threadCursor;
                    threadCursor = (threadCursor + 1) % presents.size();
#               if defined(__EMSCRIPTEN__)
                    // Decided once this present's workload is done (below).
                    newerQueued = false;
#               else
                    newerQueued = (writeCursor != threadCursor);
#               endif
                }
            }

            // present-stats: a game frame ended at each present the thread came to without waiting for it, unless the one
            // before was dropped (a second of a burst, reported without the frames).
            if (presentStats && (processCursor >= 0)) {
                if (waited) {
                    resetBusyFrames();
                }
                else if (!lastPresentDropped) {
                    busyFrames++;
                }
            }

            if (processCursor >= 0) {
                std::unique_lock<std::mutex> threadLock(threadMutex);
                const bool needsResize = ext.swapChain->needsResize() || !swapChainValid;
                if (needsResize) {
                    ext.presentGraphicsWorker->commandList->begin();
                    ext.presentGraphicsWorker->commandList->end();
                    ext.presentGraphicsWorker->execute();
                    ext.presentGraphicsWorker->wait();
                    swapChainValid = ext.swapChain->resize();
                    swapChainFramebuffers.clear();

                    if (swapChainValid) {
                        ext.sharedResources->setSwapChainSize(ext.swapChain->getWidth(), ext.swapChain->getHeight());
                        
                        // Texture count could've changed after resize, so new semaphores are needed.
                        while (drawSemaphores.size() < ext.swapChain->getTextureCount()) {
                            drawSemaphores.emplace_back(ext.device->createCommandSemaphore());
                        }
                    }
                }

                if (needsResize || ext.appWindow->detectWindowMoved()) {
                    ext.appWindow->detectRefreshRate();
                    ext.sharedResources->setSwapChainRate(std::min(ext.appWindow->getRefreshRate(), displayTimingRate));
                }

                if (displayTiming) {
                    uint32_t newDisplayTimingRate = ext.swapChain->getRefreshRate();
                    if (newDisplayTimingRate == 0) {
                        newDisplayTimingRate = UINT32_MAX;
                    }

                    if (newDisplayTimingRate != displayTimingRate) {
                        ext.sharedResources->setSwapChainRate(std::min(ext.appWindow->getRefreshRate(), newDisplayTimingRate));
                        displayTimingRate = newDisplayTimingRate;
                    }
                }

                skipPresent = newerQueued || ext.swapChain->isEmpty();

#           if defined(__EMSCRIPTEN__)
                // A hidden page shows nothing, and in Chrome a present's wait could hold this thread for seconds there
                // (plume's isHidden()), and the game behind it, whose sound broke up. So it presents nothing until shown.
                skipPresent = skipPresent || ext.swapChain->isHidden();
#           endif

                Present &present = presents[processCursor];
                {
                    PhaseTimer phaseTimer(this, PhaseWorkload);
                    ext.workloadQueue->waitForWorkloadId(present.workloadId);
                }

                if (!presentThreadRunning) {
                    continue;
                }

#           if defined(__EMSCRIPTEN__)
                // A present is dropped for a newer one only if the newer one's workload is done too, so it can be shown in
                // its place now. Decided when this one was taken from the queue, before its workload, a page whose workloads
                // end later than the game's next present comes (a phone's CPU, slowed by its heat, at a level's start) found
                // a newer one queued each time and dropped every present: Banjo-Tooie on a Galaxy S21 went on at 15-20 game
                // frames a second showing none, for good. Workloads end in order, so the next one is the one to look at.
                {
                    const std::scoped_lock cursorLock(cursorMutex);
                    newerQueued = (writeCursor != threadCursor) && ext.workloadQueue->isWorkloadDone(presents[threadCursor].workloadId);
                }

                skipPresent = skipPresent || newerQueued;
#           endif

                if (presentStats) {
                    lastPresentDropped = newerQueued;
                    if (newerQueued) {
                        reportDroppedPresent(processCursor, threadCursor);
                    }
                }

#           if defined(__EMSCRIPTEN__)
                pace(skipPresent && !ext.swapChain->isEmpty());
#           endif

                if (skipPresent) {
                    skipInterpolation();
                    notifyPresentId(present);
                }
                else {
                    threadPresent(present, swapChainValid);
                }

                if (!present.paused) {
                    if (!present.fbOperations.empty()) {
                        const std::scoped_lock lock(screenFbChangePoolMutex);
                        screenFbChangePool.release(present.fbOperations.front().writeChanges.id);
                        present.fbOperations.clear();
                    }

                    threadAdvanceBarrier();
                }

                processCursor = -1;
            }
        }

        // Transition the active swap chain render target out of the present state to avoid live references to the resource.
        uint32_t swapChainIndex = 0;
        if (!ext.swapChain->isEmpty() && ext.swapChain->acquireTexture(acquiredSemaphore.get(), &swapChainIndex)) {
            RenderTexture *swapChainTexture = ext.swapChain->getTexture(swapChainIndex);
            ext.presentGraphicsWorker->commandList->begin();
            ext.presentGraphicsWorker->commandList->barriers(RenderBarrierStage::NONE, RenderTextureBarrier(swapChainTexture, RenderTextureLayout::COLOR_WRITE));
            ext.presentGraphicsWorker->commandList->end();

            const RenderCommandList *commandList = ext.presentGraphicsWorker->commandList.get();
            RenderCommandSemaphore *waitSemaphore = acquiredSemaphore.get();
            ext.presentGraphicsWorker->commandQueue->executeCommandLists(&commandList, 1, &waitSemaphore, 1, nullptr, 0, ext.presentGraphicsWorker->commandFence.get());
            ext.presentGraphicsWorker->wait();
        }
    }
};
