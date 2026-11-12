//
// RT64
//

#pragma once

#include <atomic>

#include "xxHash/xxh3.h"

#include "common/rt64_emulator_configuration.h"
#include "common/rt64_enhancement_configuration.h"
#include "gui/rt64_camera_controller.h"
#include "gui/rt64_debugger_inspector.h"
#include "preset/rt64_preset_draw_call.h"
#include "preset/rt64_preset_material.h"
#include "render/rt64_framebuffer_renderer.h"
#include "render/rt64_render_worker.h"
#include "render/rt64_raster_shader_cache.h"
#include "render/rt64_render_target_manager.h"
#include "render/rt64_rsp_processor.h"
#include "render/rt64_texture_cache.h"
#include "render/rt64_transform_processor.h"

#include "rt64_application_window.h"
#include "rt64_framebuffer.h"
#include "rt64_framebuffer_manager.h"
#include "rt64_game_configuration.h"
#include "rt64_light_manager.h"
#include "rt64_microcode.h"
#include "rt64_present_queue.h"
#include "rt64_rdp.h"
#include "rt64_rsp.h"
#include "rt64_rdp_tmem.h"
#include "rt64_workload_queue.h"

namespace RT64 {
    struct Application;
    struct Interpreter;
    
    const uint32_t RDRAMSize = 0x7FFFFF;

    struct State {
        struct External {
            Application *app;
            ApplicationWindow *appWindow;
            Interpreter *interpreter;
            RenderDevice *device;
            RenderSwapChain *swapChain;
            RenderWorker *framebufferGraphicsWorker;
            ShaderLibrary *shaderLibrary;
            BufferUploader *drawDataUploader;
            BufferUploader *transformsUploader;
            BufferUploader *tilesUploader;
            WorkloadQueue *workloadQueue;
            PresentQueue *presentQueue;
            SharedQueueResources *sharedQueueResources;
            RasterShaderCache *rasterShaderCache;
            TextureCache *textureCache;
            EmulatorConfiguration *emulatorConfig;
            EnhancementConfiguration *enhancementConfig;
            UserConfiguration *userConfig;
            ProfilingTimer *dlApiProfiler;
            ProfilingTimer *screenApiProfiler;
            UserConfiguration::GraphicsAPI createdGraphicsAPI;
#       if RT_ENABLED
            RaytracingConfiguration *rtConfig;
#       endif
        };

        uint8_t *RDRAM;

        // Optional (the runtime's, or nullptr): [0] counts the display lists the runtime has had read in full, and the
        // interpreter stores in [1 + offset / 256], for each 256 bytes of RDRAM's first 8 MB it reads a command from, the
        // number of the list it is reading ([0] + 1), so the runtime can hold a load from ROM that would land on commands
        // of a list not read yet.
        std::atomic<uint32_t> *displayListReads = nullptr;

        uint32_t *MI_INTR_REG;
        void (*checkInterrupts)();
        Microcode microcode;
        std::unique_ptr<RSP> rsp;
        std::unique_ptr<RDP> rdp;
        TextureManager textureManager;
        FramebufferManager framebufferManager;
        FramebufferChangePool scratchFbChangePool;
        std::unique_ptr<FramebufferRenderer> framebufferRenderer;
        RenderTargetManager renderTargetManager;
        std::unique_ptr<RenderFramebufferManager> renderFramebufferManager;
        std::unique_ptr<RSPProcessor> rspProcessor;
        std::vector<interop::PointLight> scriptLights;
        uint64_t workloadCounter;
        std::vector<uint64_t> evictedTextureHashes;
        std::unique_ptr<RenderTarget> dummyDepthTarget;
#   if RT64_WGSL
        DummyDepthTargets dummyDepthTargets;
#   endif
        DrawCall drawCall;
        DrawStatus drawStatus;
        SpriteCommand activeSpriteCommand;
        bool addLightsOnFlush;
        std::vector<FramebufferOperation> drawFbOperations;
        std::vector<uint32_t> drawFbDiscards;
        std::vector<DisplayList *> returnAddressStack;
        std::vector<Framebuffer *> differentFbs;

        // Render to RAM copies waiting for the next list or screen update (renderToRAMLate), in the order they were drawn.
        struct PendingRAMCopy {
            uint32_t address;
            uint32_t rowWidth;
            uint32_t rowStart;
            uint32_t rowEnd;
        };

        // For each framebuffer with a pending copy, by its address: the words of RDRAM it covers (from a word-aligned
        // start) as they were when the list ended, with each copy written in as it lands, and whether the CPU has
        // written to them since.
        struct PendingRAMImage {
            uint32_t start = 0;
            std::vector<uint32_t> words;
            bool checked = false;
            bool cpuWrote = false;
        };

        std::vector<PendingRAMCopy> pendingRAMCopies;
        std::unordered_map<uint32_t, PendingRAMImage> pendingRAMImages;
        uint32_t lateRAMCopyCpuWrites = 0;

        // Set by the RDP's full sync while fullSync() runs if the command after it ends the list at its top level (and
        // the list then ends there, that command not read again): only then has the RSP read every command of the list
        // by the time the RDP is done, so the game may be told so before the last batch is recorded
        // (Application::Core::rdpDone) without the interpreter reading what it writes next.
        bool listEndsAfterFullSync = false;
        GameConfiguration gameConfig;
        uint32_t displayListAddress;
        uint64_t displayListCounter;
        bool rdramCheckPending;
        uint32_t lastWorkloadIndex;
        VI lastScreenVI;
        uint64_t lastScreenHash;
        uint32_t lastScreenFactorCounter;
        VIHistory viHistory;

        // The last different framebuffers the VI has shown, for which a workload draws to (advanceWorkload()):
        // viHistory keeps the last three VIs, and a game with three framebuffers can show its third only now and then.
        // workloadId is the last workload that drew to it, which a present of it waits for (advancePresent()).
        struct ShownFramebuffer {
            uint32_t address = 0;
            uint32_t width = 0;
            uint8_t siz = 0;
            uint64_t workloadId = 0;
        };

        std::array<ShownFramebuffer, 8> shownFramebuffers = {};
        uint32_t shownFramebufferCursor = 0;
        PresetDrawCallLibrary drawCallLibrary;
        PresetLightsLibrary lightsLibrary;
        PresetMaterialLibrary materialLibrary;
        PresetSceneLibrary sceneLibrary;
        PresetDrawCallLibraryInspector drawCallLibraryInspector;
        PresetLightsLibraryInspector lightsLibraryInspector;
        PresetMaterialLibraryInspector materialLibraryInspector;
        PresetSceneLibraryInspector sceneLibraryInspector;
        CameraController cameraController;
        DebuggerInspector debuggerInspector;
        std::stringstream scriptLog;
        ProfilingTimer dlCpuProfiler = ProfilingTimer(120);
        ProfilingTimer screenCpuProfiler = ProfilingTimer(120);
        ProfilingTimer viChangedProfiler = ProfilingTimer(120);
        std::filesystem::path dumpingTexturesDirectory;
        bool configurationSaveQueued = false;
        uint64_t workloadId = 0;
        uint64_t presentId = 0;
        uint32_t ditherRandomSeed = 0;
        uint32_t renderFlagSampleCount = 0;
        External ext;

        struct Extended {
            hlslpp::float2 texcoordWrapPoint = { 8092.0f, 8092.0f };
            uint16_t refreshRate = UINT16_MAX;
            uint8_t renderToRAM = UINT8_MAX;
            bool vertexTestZActive = false;
            float ditherNoiseStrength = 1.0f;
            bool extendRDRAM = false;
            bool pipelineWarmUp = false;
        };

        Extended extended;

        State(uint8_t *RDRAM, uint32_t *MI_INTR_REG, void (*checkInterrupts)());
        ~State();
        void setup(const External &ext);
        void reset();
        void resetDrawCall();
        void updateDrawStatusAttribute(DrawAttribute attribute);
        bool checkDrawState();
        void loadDrawState();
        void flush();
        void submitFramebufferPair(FramebufferPair::FlushReason flushReason);
        void checkRDRAM();
        void addPendingRAMCopy(Framebuffer *fb, uint32_t rowWidth, uint32_t rowStart, uint32_t rowEnd);
        void keepPendingRAMImage(Framebuffer *fb, uint32_t rowWidth, uint32_t rowEnd);
        const uint8_t *keptRAM(uint32_t address, uint32_t byteCount) const;
        void landRAMCopies(bool onlyIfReady);
        void fullSync();
        void fullSyncFramebufferPairTiles(Workload &workload, FramebufferPair &fbPair, uint32_t &loadOpCursor, uint32_t &rdpTileCursor);
        void listProcessBegin();
        void listProcessEnd();
        void updateScreen(const VI &newVI, bool fromEarlyPresent);
        void updateMultisampling();
        void inspect();
        void advanceWorkload(Workload &workload, bool paused);
        void advancePresent(Present &present, bool paused);
        void dpInterrupt();
        void spInterrupt();
        void advanceFramebufferRenderer();
        void flushFramebufferOperations(FramebufferPair &fbPair);
        bool hasFramebufferOperationsPending() const;
        void pushReturnAddress(DisplayList *dl);
        DisplayList *popReturnAddress();
        void setRefreshRate(uint16_t refreshRate);
        void setRenderToRAM(uint8_t renderToRAM);
        void warmUpPipelines();
        void setDitherNoiseStrength(float noiseStrength);
        void setExtendedRDRAM(bool isExtended);
        void setTexcoordWrapPoint(int16_t wrapU, int16_t wrapV);
        void startSpriteCommand(uint64_t replacementHash);
        void endSpriteCommand();
        uint8_t *fromRDRAM(uint32_t rdramAddress) const;
        void dumpRDRAM(const std::string &path);
        void enableExtendedGBI(uint8_t opCode);
        void disableExtendedGBI();
        void clearExtended();
        void updateRenderFlagSampleCount();
    };
};