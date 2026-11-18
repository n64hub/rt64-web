//
// RT64
//

#pragma once

#include <mutex>

#include "common/rt64_common.h"
#include "common/rt64_emulator_configuration.h"
#include "common/rt64_enhancement_configuration.h"
#include "render/rt64_render_target_manager.h"

#include "rt64_framebuffer_manager.h"
#include "rt64_frame_dump.h"

#if RT_ENABLED
#   include "render/rt64_raytracing_resources.h"
#endif

namespace RT64 {
    struct InterpolatedFrameCounters {
        uint32_t presented = 0;
        uint32_t available = 0;
        uint32_t count = 0;
        bool skipped = false;
#   if RT64_GAME_FRAME_WORKLOADS
        // Whether a present is showing these counters' targets (PresentQueue::threadPresent()). A present waits for the
        // last workload that drew what it shows (State::advancePresent()), not the newest, so it can start after the next
        // frame switched to the other counters and take those, and no present ever shows these; or it can still hold
        // its counters two frames on, the workload thread going on as soon as a present has shown its first frame. So a
        // frame switching away waits for the set before to be shown only while a present holds it, and a frame starts
        // on a set only once no present holds it: the frame that present shows has ended, so it does. Each waited for
        // good before, Mario Party 2's page 3 s into its boot, its present waiting for frames of a set reset under it.
        bool inUse = false;
#   endif
        // Whether a present has shown these counters' last frame (PresentQueue::threadPresent()). A present that takes them
        // after one has, the game's VI showing the frame again before its next was drawn, shows nothing: the screen holds
        // that last frame. It showed every frame of the set again, the frame between the game's first, so the picture went
        // back half a frame, and the targets those frames were in were already free: once a present has shown a frame, the
        // next game frame draws into its target, so the frame shown again was half of the next one drawn, or all of it.
        // Mario Golf's menus, now and then as a frame ran late, showed a character with a forearm gone for one frame, or
        // an arm where the next pose put it. Unlike presented, which a dropped present sets too (skipInterpolation()).
        bool shown = false;
    };

    struct SharedQueueResources {
        // Render configuration.
        uint32_t swapChainWidth = 0;
        uint32_t swapChainHeight = 0;
        uint32_t swapChainRate = 0;
        hlslpp::float2 resolutionScale = 1.0f;
        uint32_t targetRate = 0;
        std::atomic<uint32_t> pacedRate = 0;
        std::atomic<uint32_t> prebuildWaits = 0;
        UserConfiguration userConfig;
        EnhancementConfiguration enhancementConfig;
        EmulatorConfiguration emulatorConfig;
        bool swapChainSizeChanged = false;
        bool rtConfigChanged = true;
        bool userConfigChanged = true;
        bool fbConfigChanged = true;
        bool newConfigValidated = false;
        std::mutex configurationMutex;

        // Render resource managers.
        FramebufferManager framebufferManager;
        RenderTargetManager renderTargetManager;
        std::mutex managerMutex;

        // Workload to present state.
        uint32_t viOriginalRate = 0;
        std::vector<uint32_t> colorImageAddressVector;
        std::unordered_set<uint32_t> colorImageAddressSet;
        std::vector<std::unique_ptr<RenderTarget>> interpolatedColorTargets;
        InterpolatedFrameCounters interpolatedFrames[2];
        uint32_t interpolatedFramesIndex = 0;
        std::mutex interpolatedMutex;
        std::condition_variable interpolatedCondition;
        std::mutex workloadMutex;
#   if defined(__EMSCRIPTEN__)
        // Whether the game's list is in its full sync, from GBI_RDP::fullSync()'s ListGPUWait until
        // State::advanceWorkload() queues its workload: its waits for the device thread (a readback of what it drew
        // into RDRAM, a buffer made larger) wait for every WebGPU call sent before them, so the workload thread starts
        // no interpolated frame meanwhile, for a while at most (waitForListGPUWaits()). It ends before the workload is
        // queued, since that waits for the workload thread when the queue is full, and so do the present queue's waits
        // after it.
        bool listInFullSync = false;
        std::mutex listGPUWaitMutex;
        std::condition_variable listGPUWaitCondition;

        void listGPUWaitBegin() {
            std::scoped_lock<std::mutex> listLock(listGPUWaitMutex);
            listInFullSync = true;
        }

        // Ends it if it hasn't ended.
        void listGPUWaitEnd() {
            {
                std::scoped_lock<std::mutex> listLock(listGPUWaitMutex);
                if (!listInFullSync) {
                    return;
                }

                listInFullSync = false;
            }

            listGPUWaitCondition.notify_all();
        }

        // Until the game's list isn't in its full sync, or maxMs have gone by.
        void waitForListGPUWaits(uint32_t maxMs) {
            std::unique_lock<std::mutex> listLock(listGPUWaitMutex);
            listGPUWaitCondition.wait_for(listLock, std::chrono::milliseconds(maxMs), [this]() {
                return !listInFullSync;
            });
        }

        // The game's list in its full sync for as long as one lives, unless it ends first.
        struct ListGPUWait {
            SharedQueueResources *resources;

            ListGPUWait(SharedQueueResources *resources) : resources(resources) {
                resources->listGPUWaitBegin();
            }

            ~ListGPUWait() {
                resources->listGPUWaitEnd();
            }
        };
#   endif

        void setSwapChainSize(uint32_t width, uint32_t height) {
            std::scoped_lock<std::mutex> configurationLock(configurationMutex);
            if ((swapChainWidth != width) || (swapChainHeight != height)) {
                swapChainWidth = width;
                swapChainHeight = height;
                swapChainSizeChanged = true;
            }
        }

        void setSwapChainRate(uint32_t rate) {
            std::scoped_lock<std::mutex> configurationLock(configurationMutex);
            swapChainRate = FrameDump::swapChainRate(rate);
        }

        void setUserConfig(const UserConfiguration &newUserConfig, bool discardFBs) {
            std::scoped_lock<std::mutex> configurationLock(configurationMutex);
            userConfig = newUserConfig;
            userConfigChanged = true;
            fbConfigChanged = discardFBs;
            newConfigValidated = false;
        }

        void setEmulatorConfig(const EmulatorConfiguration &newEmulatorConfig) {
            std::scoped_lock<std::mutex> configurationLock(configurationMutex);
            emulatorConfig = newEmulatorConfig;
        }

        void setEnhancementConfig(const EnhancementConfiguration &newEnhancementConfig) {
            std::scoped_lock<std::mutex> configurationLock(configurationMutex);
            enhancementConfig = newEnhancementConfig;
        }

        void updateMultisampling(const RenderMultisampling &multisampling) {
            interpolatedFrames[0].skipped = true;
            interpolatedFrames[1].skipped = true;
            renderTargetManager.destroyAll();
            renderTargetManager.setMultisampling(multisampling);
        }

#   if RT_ENABLED
        RaytracingConfiguration rtConfig;

        void setRtConfig(const RaytracingConfiguration &newRtConfig) {
            std::scoped_lock<std::mutex> configurationLock(configurationMutex);
            rtConfig = newRtConfig;
            rtConfigChanged = true;
        }
#   endif
    };
};