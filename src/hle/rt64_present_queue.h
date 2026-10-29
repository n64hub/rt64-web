//
// RT64
//

#pragma once

#include <array>
#include <deque>
#include <optional>

#include "common/rt64_profiling_timer.h"
#include "gui/rt64_inspector.h"
#include "render/rt64_vi_renderer.h"

#include "rt64_application_window.h"
#include "rt64_present.h"
#include "rt64_shared_queue_resources.h"

#define PRESENT_QUEUE_SIZE 4

namespace RT64 {
    struct WorkloadQueue;

    struct PresentQueue {
        struct External {
            ApplicationWindow *appWindow = nullptr;
            RenderDevice *device = nullptr;
            RenderSwapChain *swapChain = nullptr;
            RenderWorker *presentGraphicsWorker = nullptr;
            WorkloadQueue *workloadQueue = nullptr;
            SharedQueueResources *sharedResources = nullptr;
            const ShaderLibrary *shaderLibrary = nullptr;
            UserConfiguration::GraphicsAPI createdGraphicsAPI = UserConfiguration::GraphicsAPI::OptionCount;
        };

        External ext;
        std::array<Present, PRESENT_QUEUE_SIZE> presents;
        int threadCursor;
        int writeCursor;
        int barrierCursor;
        std::mutex cursorMutex;
        std::condition_variable cursorCondition;
        uint64_t presentId;
        std::mutex presentIdMutex;
        std::condition_variable presentIdCondition;
        std::thread *presentThread = nullptr;
        std::mutex threadMutex;
        std::atomic<bool> presentThreadRunning = false;
        std::recursive_mutex inspectorMutex;
        std::mutex screenFbChangePoolMutex;
        Framebuffer scratchFb;
        FramebufferChangePool scratchFbChangePool;
        FramebufferChangePool screenFbChangePool;
        std::atomic<bool> viewRDRAM = false;
        std::vector<std::unique_ptr<RenderFramebuffer>> swapChainFramebuffers;
        std::unique_ptr<RenderCommandSemaphore> acquiredSemaphore;
        std::vector<std::unique_ptr<RenderCommandSemaphore>> drawSemaphores;
        std::unique_ptr<VIRenderer> viRenderer;
        std::unique_ptr<Inspector> inspector;
        ProfilingTimer presentProfiler = ProfilingTimer(120);
        Timestamp presentTimestamp;
        VIHistory viHistory;
        bool presentWaitEnabled = false;
        uint32_t pacingMultiple = 0;
        std::deque<std::pair<Timestamp, bool>> pacingPresents;
        uint32_t pacingDropped = 0;
        Timestamp pacingChanged;
        Timestamp pacingRaised;
        int64_t pacingRetryMicro = 0;
        uint32_t pacingPrebuildWaits = 0;
        Timestamp pacingWaited;
        bool uncapped = false;
        Timestamp presentSlot;

        // present-stats (?present-stats): when each present was queued, and where the present thread's time went in the
        // game frames since it last waited for one or dropped one, which it prints at each one it drops (threadLoop()).
        enum PresentPhase { PhaseWorkload, PhaseInterpolated, PhaseDraw, PhaseSleep, PhaseWait, PhasePresent, PhaseCount };
        bool presentStats = false;
        std::array<Timestamp, PRESENT_QUEUE_SIZE> presentQueued;
        std::array<int64_t, PhaseCount> busyPhaseMicro = {};
        int64_t busyLateMicro = 0;
        uint32_t busyFrames = 0;
        bool lastPresentDropped = false;
        Timestamp busyStart;

        // Adds the time until the end of its scope to a phase, while present-stats is on.
        struct PhaseTimer {
            PresentQueue *queue;
            PresentPhase phase;
            Timestamp start;

            PhaseTimer(PresentQueue *queue, PresentPhase phase);
            ~PhaseTimer();
        };

        PresentQueue();
        ~PresentQueue();
        void reset();
        void advanceToNextPresent();
        void repeatLastPresent();
        uint32_t previousWriteCursor() const;
        void waitForIdle();
        void waitForPresentId(uint64_t waitId);
        void setup(const External &ext);
        void threadPresent(const Present &present, bool &swapChainValid);
        void skipInterpolation();
        void pace(bool presentDropped);
        void resetBusyFrames();
        void reportDroppedPresent(int droppedCursor, int nextCursor);
        void notifyPresentId(const Present &present);
        void threadAdvanceBarrier();
        void threadLoop();
    };
};