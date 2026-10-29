//
// RT64
//

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <tuple>
#include <string>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/rt64_elapsed_timer.h"
#include "rt64_raster_shader.h"

namespace RT64 {
    struct RasterShaderCache {
        struct CompilationThread {
            RasterShaderCache *shaderCache;
            std::unique_ptr<std::thread> thread;
            std::atomic<bool> threadRunning;

            CompilationThread(RasterShaderCache *shaderCache);
            ~CompilationThread();
            void loop();
        };

        RenderDevice *device;
        std::unique_ptr<RasterShaderUber> shaderUber;
        OptimizerCacheSPIRV optimizerCacheSPIRV;
        ShaderCacheWGSL shaderCacheWGSL;
        std::mutex submissionMutex;
        std::queue<ShaderDescription> descQueue;
        std::mutex descQueueMutex;
        int32_t descQueueActiveCount = 0;
        std::condition_variable descQueueChanged;
        std::unordered_map<uint64_t, bool> shaderHashes;
        std::unordered_map<uint64_t, std::shared_ptr<RasterShader>> GPUShaders;
        std::mutex GPUShadersMutex;
        std::list<std::unique_ptr<CompilationThread>> compilationThreads;
        uint32_t threadCount;
        uint32_t ubershaderThreadCount;
        RenderShaderFormat shaderFormat;
        std::unique_ptr<ShaderCompiler> shaderCompiler;
        RenderMultisampling multisampling;
        bool usesHDR = false;
        bool specializedShaders = true;
        bool prebuiltShaders = false;
        bool recordShaders = false;

        // The shaders specialized when the program was built, in sections (startPrebuild()): a round builds the
        // shaders a frame needs with the game waiting, and the rest are built ahead of it (prebuildAhead), the
        // section the game is in first. The ones the list missed on an
        // earlier visit are learned (learnPrebuild()), and descriptions that make the same pipeline share one built
        // for the first of them (prebuildAliases).
        std::list<std::thread> prebuildThreads;
        std::mutex prebuildMutex;
        std::condition_variable prebuildChanged;
        bool prebuildStop = false;
        uint32_t prebuildCount = 0;
        std::atomic<uint32_t> prebuildBuilt = 0;
        enum : uint8_t { PrebuildQueued, PrebuildWanted, PrebuildSoon, PrebuildTaken };
        std::vector<uint8_t> prebuildState;
        std::vector<uint32_t> prebuildWanted;
        std::unordered_map<uint64_t, uint32_t> prebuildIndices;
        std::vector<uint32_t> prebuildRound;
        uint32_t prebuildRoundNext = 0;
        uint32_t prebuildRoundDone = 0;
        bool prebuildRoundHeld = false;
        uint64_t prebuildRoundSize = 0;
        uint64_t prebuildRoundSizeDone = 0;
        uint64_t prebuildRoundSizeStarted = 0;
        std::deque<uint32_t> prebuildAhead;
        std::vector<bool> prebuildBoot;
        uint32_t prebuildBootThreads = 1;
        std::atomic<bool> prebuildDrawn = false;
        std::string prebuildText;
        ElapsedTimer prebuildTimer;
        std::vector<ShaderDescription> prebuildLearned;
        std::vector<std::vector<uint32_t>> prebuildLearnedSections;
        std::vector<std::vector<uint32_t>> prebuildSections;
        std::vector<std::vector<uint64_t>> prebuildAliases;
        std::atomic<uint32_t> prebuildSection = 0;

        // What the list missed where nothing is built during play (Firefox), built before the next frame (buildMissed()).
        std::deque<ShaderDescription> missedQueue;
        std::mutex missedMutex;

        RasterShaderCache(uint32_t threadCount, uint32_t ubershaderThreadCount);
        ~RasterShaderCache();
        void setup(RenderDevice *device, RenderShaderFormat shaderFormat, const ShaderLibrary *shaderLibrary, const RenderMultisampling &multisampling);
        void submit(const ShaderDescription &desc);
        void waitForAll();
        void startPrebuild();
        void startPrebuildRound(uint32_t section, std::unique_lock<std::mutex> &lock);
        void prebuildLoop(uint32_t thread);
        void wantPrebuild(uint64_t shaderHash);
        void reportPrebuild(uint32_t done, uint32_t count, uint64_t sizeDone, uint64_t sizeStarted, uint64_t size);
        bool waitForPrebuild();
        void learnPrebuild();
        void reportMissed(const ShaderDescription &desc);
        bool buildMissed();
        void destroyAll();
        RasterShader *getGPUShader(const ShaderDescription &desc);
        RasterShaderUber *getGPUShaderUber() const;
        uint32_t shaderCount();
    };
};