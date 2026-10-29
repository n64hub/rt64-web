//
// RT64
//

#include "rt64_raster_shader_cache.h"

#include "common/rt64_thread.h"

#if defined(__EMSCRIPTEN__)
#   include <emscripten/emscripten.h>
#endif

#if defined(RT64_PREBUILT_SHADERS)
#   include <cstring>
#   include <zstd.h>
#   include "common/rt64_elapsed_timer.h"
#   include "rt64_prebuilt_shaders.h"
#   include "rt64_render_target.h"

// The size of the pixel shader built for a prebuilt shader, on a device with dual-source blending or without.
static uint32_t prebuiltPixelSize(const RT64PrebuiltShader &entry, bool dualSource) {
    return dualSource ? entry.pixelSize : entry.pixelSingleSize;
}

// The WGSL built for a prebuilt shader, which goes with the time it takes.
static uint64_t prebuiltSize(const RT64PrebuiltShader &entry, bool dualSource) {
    return uint64_t(entry.vertexSize) + prebuiltPixelSize(entry, dualSource);
}

static RT64::ShaderDescription prebuiltDescription(const RT64PrebuiltShader &entry) {
    RT64::ShaderDescription desc;
    memset(&desc, 0, sizeof(desc));
    desc.otherMode.L = entry.otherModeL;
    desc.otherMode.H = entry.otherModeH;
    desc.colorCombiner.L = entry.combinerL;
    desc.colorCombiner.H = entry.combinerH;
    desc.flags.value = entry.flags;
    return desc;
}

// How many of a section's shaders a frame must want for the game to wait for the section (startPrebuild()).
static const uint32_t PrebuildSectionMinimum = 4;

// The most shaders a page remembers the list missed (learnPrebuild()).
static const uint32_t LearnedMaximum = 1000;

// A learned shader is built from the ubershader's WGSL with its spec constants as overrides, which takes longer than
// a prebuilt one: it counts as the largest, for the progress and for its place in a round.
static const uint64_t LearnedSize = 400000;
#endif

// On WebGPU a specialized RasterPS takes 1.4-2 s to build in the browser's GPU process (Chrome, RTX 2070). plume's
// WebGPU backend builds it with createRenderPipelineAsync and the compilation thread waits for it, so the ubershader
// draws meanwhile, as everywhere else.
#define ENABLE_OPTIMIZED_SHADER_GENERATION

namespace RT64 {
    // RasterShaderCache::CompilationThread
    
    RasterShaderCache::CompilationThread::CompilationThread(RasterShaderCache *shaderCache) {
        assert(shaderCache != nullptr);

        this->shaderCache = shaderCache;

        thread = std::make_unique<std::thread>(&CompilationThread::loop, this);
        threadRunning = false;
    }

    RasterShaderCache::CompilationThread::~CompilationThread() {
        threadRunning = false;
        shaderCache->descQueueChanged.notify_all();
        thread->join();
        thread.reset(nullptr);
    }

    void RasterShaderCache::CompilationThread::loop() {
        Thread::setCurrentThreadName("RT64 Shader");

        // The shader compilation thread should have idle priority by default as the application can use the ubershader in the meantime.
        Thread::setCurrentThreadPriority(Thread::Priority::Idle);

        threadRunning = true;

        while (threadRunning) {
            ShaderDescription shaderDesc;
            bool fromPriorityQueue = false;
            
            // Check the top of the queue or wait if it's empty.
            {
                std::unique_lock<std::mutex> queueLock(shaderCache->descQueueMutex);
                shaderCache->descQueueActiveCount--;
                shaderCache->descQueueChanged.wait(queueLock, [this]() {
                    return !threadRunning || !shaderCache->descQueue.empty();
                });

                shaderCache->descQueueActiveCount++;
                if (!shaderCache->descQueue.empty()) {
                    shaderDesc = shaderCache->descQueue.front();
                    shaderCache->descQueue.pop();
                    fromPriorityQueue = true;
                }
            }
            
            // Compile the shader at the top of the queue.
            if (fromPriorityQueue) {
                assert((shaderCache->shaderUber != nullptr) && "Ubershader should've been created by the time a new shader is submitted to the cache.");
                const RenderPipelineLayout *uberPipelineLayout = shaderCache->shaderUber->pipelineLayout.get();
                const RenderMultisampling multisampling = shaderCache->multisampling;
                std::unique_ptr<RasterShader> newShader = std::make_unique<RasterShader>(shaderCache->device, shaderDesc, uberPipelineLayout, shaderCache->shaderFormat, multisampling, shaderCache->shaderCompiler.get(), &shaderCache->optimizerCacheSPIRV, &shaderCache->shaderCacheWGSL);

                {
                    const std::unique_lock<std::mutex> lock(shaderCache->GPUShadersMutex);
                    shaderCache->GPUShaders[shaderDesc.hash()] = std::move(newShader);
                }
            }
        }
    }

    // RasterShaderCache

    RasterShaderCache::RasterShaderCache(uint32_t threadCount, uint32_t ubershaderThreadCount) {
        assert(threadCount > 0);

        this->threadCount = threadCount;
        this->ubershaderThreadCount = ubershaderThreadCount;

#ifdef ENABLE_OPTIMIZED_SHADER_GENERATION
#   ifdef _WIN32
        shaderCompiler = std::make_unique<ShaderCompiler>();
#   endif

        descQueueActiveCount = threadCount;

        for (uint32_t t = 0; t < threadCount; t++) {
            compilationThreads.push_back(std::make_unique<CompilationThread>(this));
        }
#endif
    }

    RasterShaderCache::~RasterShaderCache() {
        compilationThreads.clear();
        {
            std::unique_lock<std::mutex> lock(prebuildMutex);
            prebuildStop = true;
        }

        prebuildChanged.notify_all();
        for (std::thread &thread : prebuildThreads) {
            thread.join();
        }
    }

    void RasterShaderCache::setup(RenderDevice *device, RenderShaderFormat shaderFormat, const ShaderLibrary *shaderLibrary, const RenderMultisampling &multisampling) {
        assert(device != nullptr);

        this->device = device;
        this->shaderFormat = shaderFormat;
        this->multisampling = multisampling;

        shaderUber = std::make_unique<RasterShaderUber>(device, shaderFormat, multisampling, shaderLibrary, ubershaderThreadCount);
        usesHDR = shaderLibrary->usesHDR;

        // Where building a pipeline holds up the device (WebGPU in Firefox), a specialized shader stalls the frames
        // for longer than the ubershader it replaces costs them, and a race asks for a new one every few seconds.
        // What isn't prebuilt is built with the game waiting there (buildMissed()), never behind it.
        specializedShaders = device->getCapabilities().backgroundPipelineBuilds;

        // The prebuilt shaders are built wherever a game has a list, whatever the browser and the adapter: they are
        // built before the game starts, when holding up the device costs nothing, and without them a weak GPU draws
        // everything with the ubershader, a frame a second (startPrebuild()).
#   if defined(RT64_PREBUILT_SHADERS)
        prebuiltShaders = (shaderFormat == RenderShaderFormat::WGSL) && (multisampling.sampleCount == 1);
#   endif

#   if defined(__EMSCRIPTEN__)
        // A page opened with ?record-shaders prints every shader the game asks for, for a list of prebuilt shaders
        // (RT64_PREBUILT_SHADER_LIST in CMakeLists.txt says how it is used).
        recordShaders = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).has('record-shaders') ? 1 : 0; }) != 0;
#   endif

        // Initialize the re-spirv optimizer cache.
        if (shaderFormat == RenderShaderFormat::SPIRV) {
            optimizerCacheSPIRV.initialize(device->getCapabilities());
        }

        // Create the spec constant shaders before anything draws. plume's WebGPU backend learns from each new shader which
        // stages read which bindings, and when that changes every pipeline gets a new layout: the specialized ones that
        // were drawn with would then be built again in place, holding up the browser's GPU process for seconds. Without
        // specialized or prebuilt shaders nothing needs them, and RasterPS is 250 KB of WGSL for the browser to parse.
        if ((shaderFormat == RenderShaderFormat::WGSL) && (specializedShaders || prebuiltShaders)) {
            const RenderShader *vertexShader = nullptr;
            const RenderShader *pixelShader = nullptr;
            shaderCacheWGSL.get(device, multisampling.sampleCount > 1, vertexShader, pixelShader);
        }

        startPrebuild();
    }

    // The prebuilt shaders are the ones a game recorded (?record-shaders), specialized when the program was built and
    // built here before the game asks for them or with it waiting, so a weak GPU never draws a frame mostly with the
    // ubershader, which costs several times what they do per pixel (on a laptop's Intel graphics it was the difference
    // between a frame a second and 60), and no shader is built in the middle of play where that would stall it. Built from the ubershader's WGSL with the spec constants as overrides, a
    // specialized RasterPS took 6 s cold in Chrome (RTX 2070); specialized beforehand, 1-1.6 s. Chrome keeps them in its
    // WebGPU cache, so a later visit builds them in a fraction of the time (Chrome 154: Rush 2049's 105 in 0.4 s). That
    // cache holds 6 MB a profile, every site's together: past it, the least recently used go.
    //
    // The list is in sections, a game's screens or levels (rt64_prebuilt_shaders.h), so a game with many levels doesn't
    // keep the player waiting for all of them at the start: GoldenEye's 455, its menus and every level's start, are
    // 2.5 s for the menus and 6.6 s for Dam, cold (2026-10-13, Chrome 155 headless, RTX 2070, 12 threads). Every
    // prebuilt shader a frame asks for that isn't built is wanted (wantPrebuild()), and before the next frame
    // waitForPrebuild() looks for the section that holds the most of them: if that is at least PrebuildSectionMinimum,
    // the game has gone somewhere new and a round builds them, the game waiting. Fewer are strays, a shader one level
    // shares with another that the recording didn't see there, built behind the game, which draws them with the
    // ubershader meanwhile: a wait for one shader would cost more than it saves.
    //
    // Where pipelines build in the background (Chrome), the game waits only for what a frame wants. One thread builds
    // the whole list behind the game from the first draw (prebuildAhead), a section at a time in the list's order, the
    // order the recording reached them in; a round moves the rest of its section to the front, and strays go before
    // that. Section 0, the boot, has only a few seconds of logos before the game wants the rest, and section 1 is
    // usually what follows it by itself or a press of Start later (the attract demo, the menus), so half the threads
    // build those two; the rest is built one at a time, since each thread takes a core in the browser's GPU process and
    // levels come minutes apart. With section 1 on one thread, a list whose boot ends at the title (Pilotwings 64's,
    // so that Firefox draws the title before it builds the menus) waited 3-8 s in Chrome on the menus' 28-36 after Start,
    // where with them in the boot it never had. So the first frame waits only for its own shaders, the panel the page showed for a whole section before the
    // game started is mostly gone, and a level reached after a minute of play is usually built before the game gets
    // there. Where building a pipeline holds up the device (Firefox, setup()), anything built behind the game would
    // stall its frames, so nothing is: section 0 is built before the game gets past its first frame, a round builds
    // every shader of its section not yet built, and the game waits for strays too.
    //
    // A pipeline is built for the WebGPU layout a draw with its pipeline layout resolved (plume_webgpu.h), so section 0
    // waits for the first frame's draws; where pipelines build in the background (Chrome) that is the draw of nothing
    // at setup (State::warmUpPipelines()), and the list builds behind the launcher. A round is built on as many threads as the host has cores (2-16), since the
    // game waits and the browser's GPU process has nothing else to do, largest first: a shader's WGSL size goes with
    // its build time, and the largest (370 KB against a median of 65 KB) started last would leave the other threads idle
    // while it finished. Rush 2049's 105 took 15.6-16.7 s six at a time in the list's order, and 6.9-7.2 s twelve at a
    // time largest first (2026-10-12, Chrome 154 headless, RTX 2070, 12 cores). Each one of a round done is reported
    // to the page as Module.rt64ShaderProgress(done, count, fraction) for it to show, the fraction by WGSL size, the
    // nearest thing to time there is, with each one in flight counted half done: counting only the ones done, the
    // first seconds read as a third of the pace they were, since every thread starts on one of the largest. They are
    // made without MSAA, as the web draws, with RasterPS both with dual-source blending and without, for a device that
    // lacks it. A page opened with ?record-shaders prebuilds none, so that it prints every shader the game asks for, in
    // the order it asks.
#   if defined(RT64_PREBUILT_SHADERS)
    // The WGSL a round builds for a prebuilt or learned shader.
    static uint64_t roundSize(uint32_t index, bool dualSource) {
        return (index < RT64PrebuiltShaderCount) ? prebuiltSize(RT64PrebuiltShaderList[index], dualSource) : LearnedSize;
    }
#   endif

    void RasterShaderCache::startPrebuild() {
#   if defined(RT64_PREBUILT_SHADERS)
        if (!prebuiltShaders || recordShaders) {
            return;
        }

        prebuildText.resize(RT64PrebuiltShaderTextSize);
        const size_t textSize = ZSTD_decompress(prebuildText.data(), prebuildText.size(), RT64PrebuiltShaderData, RT64PrebuiltShaderDataSize);
        if (textSize != RT64PrebuiltShaderTextSize) {
            fprintf(stderr, "RT64: the prebuilt shaders didn't decompress: %s\n", ZSTD_isError(textSize) ? ZSTD_getErrorName(textSize) : "wrong size");
            return;
        }

        // What is prebuilt is never queued for the compilation threads. Descriptions that differ only in what their
        // shaders ignore get the same shaders (the specializer's spirv-opt), and many of those the same pipeline state
        // too: GoldenEye's 455 are 193 pipelines, Dam's 108 69. Each pipeline is built once, for the first description
        // in the list that has it, and the others' hashes are its aliases, which draw with it. Building them all, a
        // round largest first started the copies of one pipeline side by side, and the browser built each one again:
        // Dam's round took 9.6 s cold against 6.6 s (measured as above). Chrome's cache held as much either way, since
        // it keeps a pipeline once whoever asks for it, so a later visit gains nothing.
        std::vector<uint32_t> primaries(RT64PrebuiltShaderCount);
        uint32_t pipelineCount = 0;
        {
            const bool dualSource = device->getCapabilities().dualSourceBlending;
            std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> pipelines;
            prebuildAliases.resize(RT64PrebuiltShaderCount);
            std::unique_lock<std::mutex> queueLock(submissionMutex);
            for (uint32_t i = 0; i < RT64PrebuiltShaderCount; i++) {
                const RT64PrebuiltShader &entry = RT64PrebuiltShaderList[i];
                const ShaderDescription desc = prebuiltDescription(entry);
                const uint32_t pixelOffset = dualSource ? entry.pixelOffset : entry.pixelSingleOffset;
                const auto pipeline = pipelines.emplace(std::make_tuple(entry.vertexOffset, pixelOffset, RasterShader::pipelineStateKey(desc)), i);
                const uint64_t shaderHash = desc.hash();
                primaries[i] = pipeline.first->second;
                shaderHashes[shaderHash] = true;
                prebuildIndices[shaderHash] = primaries[i];
                if (pipeline.second) {
                    pipelineCount++;
                }
                else {
                    prebuildAliases[primaries[i]].emplace_back(shaderHash);
                }
            }

            learnPrebuild();
        }

        // A section's pipelines, and what was learned for it.
        prebuildSections.resize(RT64PrebuiltShaderSectionCount);
        std::vector<uint32_t> sectionOf(RT64PrebuiltShaderCount, UINT32_MAX);
        for (uint32_t s = 0; s < RT64PrebuiltShaderSectionCount; s++) {
            const RT64PrebuiltShaderSection &entries = RT64PrebuiltShaderSections[s];
            for (uint32_t i = 0; i < entries.count; i++) {
                const uint32_t index = primaries[RT64PrebuiltShaderSectionEntries[entries.first + i]];
                if (sectionOf[index] != s) {
                    sectionOf[index] = s;
                    prebuildSections[s].emplace_back(index);
                }
            }

            if (s < prebuildLearnedSections.size()) {
                prebuildSections[s].insert(prebuildSections[s].end(), prebuildLearnedSections[s].begin(), prebuildLearnedSections[s].end());
            }
        }

        prebuildCount = pipelineCount + uint32_t(prebuildLearned.size());
        prebuildState.assign(RT64PrebuiltShaderCount + prebuildLearned.size(), PrebuildQueued);
        prebuildBoot.assign(prebuildState.size(), false);
        for (uint32_t s = 0; s < std::min<uint32_t>(uint32_t(prebuildSections.size()), 2U); s++) {
            for (uint32_t index : prebuildSections[s]) {
                prebuildBoot[index] = true;
            }
        }

        const uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 2U, 16U);
        prebuildBootThreads = std::max(threads / 2, 1U);
        {
            std::unique_lock<std::mutex> lock(prebuildMutex);
            if (specializedShaders) {
                for (const std::vector<uint32_t> &section : prebuildSections) {
                    prebuildAhead.insert(prebuildAhead.end(), section.begin(), section.end());
                }

                for (uint32_t i = 0; i < RT64PrebuiltShaderCount; i++) {
                    if ((primaries[i] == i) && (sectionOf[i] == UINT32_MAX)) {
                        prebuildAhead.emplace_back(i);
                    }
                }
            }
            else {
                startPrebuildRound(0, lock);
            }
        }

        for (uint32_t t = 0; t < threads; t++) {
            prebuildThreads.emplace_back(&RasterShaderCache::prebuildLoop, this, t);
        }

        if (specializedShaders) {
            printf("RT64: %u prebuilt shaders, %u pipelines, in %u sections, and %u learned; built behind the game once it draws (sections %s and %s %u at a time, the rest one), the game waiting only for what a frame wants, %u at a time\n",
                RT64PrebuiltShaderCount, pipelineCount, RT64PrebuiltShaderSectionCount, uint32_t(prebuildLearned.size()), RT64PrebuiltShaderSections[0].name,
                (RT64PrebuiltShaderSectionCount > 1) ? RT64PrebuiltShaderSections[1].name : "(none)", prebuildBootThreads, threads);
        }
        else {
            printf("RT64: %u prebuilt shaders, %u pipelines, in %u sections, and %u learned; the game waiting for the %u of section %s, built %u at a time once the game draws\n",
                RT64PrebuiltShaderCount, pipelineCount, RT64PrebuiltShaderSectionCount, uint32_t(prebuildLearned.size()), uint32_t(prebuildRound.size()),
                RT64PrebuiltShaderSections[0].name, threads);
        }
#   endif
    }

    // A shader the list missed costs the player a moment of the ubershader and a build behind the game, so the page
    // remembers it, with the section the game was in, and the next visit builds it with that section, the game
    // waiting, as if the list had it: each player's gaps fill themselves in, whatever the game, with no more recording.
    // RT64 asks the page for what it remembers as Module.rt64LearnedShaders(list), an array of [section, other mode L
    // and H, combiner L and H, flags], and tells it of a new one as Module.rt64ShaderMissed(list, entry); `list` is a
    // number that changes with the list, since a section's index means nothing to another one. N64Recomp-browser's
    // shell keeps them in localStorage. Called with submissionMutex held.
    void RasterShaderCache::learnPrebuild() {
#   if defined(RT64_PREBUILT_SHADERS) && defined(__EMSCRIPTEN__)
        std::vector<uint32_t> learned(LearnedMaximum * 6);
        const uint32_t listId = RT64PrebuiltShaderDataSize * 31U + RT64PrebuiltShaderCount;
        const int count = MAIN_THREAD_EM_ASM_INT({
            const learned = Module['rt64LearnedShaders'] ? Module['rt64LearnedShaders']($0 >>> 0) : [];
            const count = Math.min(learned.length, $2);
            for (let i = 0; i < count; i++) {
                for (let j = 0; j < 6; j++) {
                    HEAPU32[($1 >> 2) + i * 6 + j] = learned[i][j];
                }
            }

            return count;
        }, listId, learned.data(), LearnedMaximum);

        prebuildLearnedSections.resize(RT64PrebuiltShaderSectionCount);
        for (int i = 0; i < count; i++) {
            const uint32_t *entry = &learned[i * 6];
            if (entry[0] >= RT64PrebuiltShaderSectionCount) {
                continue;
            }

            ShaderDescription desc;
            memset(&desc, 0, sizeof(desc));
            desc.otherMode.L = entry[1];
            desc.otherMode.H = entry[2];
            desc.colorCombiner.L = entry[3];
            desc.colorCombiner.H = entry[4];
            desc.flags.value = entry[5];
            const uint64_t shaderHash = desc.hash();
            if (prebuildIndices.find(shaderHash) != prebuildIndices.end()) {
                continue;
            }

            const uint32_t index = RT64PrebuiltShaderCount + uint32_t(prebuildLearned.size());
            prebuildLearned.emplace_back(desc);
            prebuildLearnedSections[entry[0]].emplace_back(index);
            prebuildIndices[shaderHash] = index;
            shaderHashes[shaderHash] = true;
        }
#   endif
    }

    // A draw asked for a shader the list doesn't have (learnPrebuild()).
    void RasterShaderCache::reportMissed(const ShaderDescription &desc) {
#   if defined(RT64_PREBUILT_SHADERS) && defined(__EMSCRIPTEN__)
        const uint32_t listId = RT64PrebuiltShaderDataSize * 31U + RT64PrebuiltShaderCount;
        MAIN_THREAD_ASYNC_EM_ASM({
            if (Module['rt64ShaderMissed']) Module['rt64ShaderMissed']($0 >>> 0, [$1 >>> 0, $2 >>> 0, $3 >>> 0, $4 >>> 0, $5 >>> 0, $6 >>> 0]);
        }, listId, prebuildSection.load(), desc.otherMode.L, desc.otherMode.H, desc.colorCombiner.L, desc.colorCombiner.H, desc.flags.value);
#   endif
    }

    // Starts a round: every shader wanted, the game waiting, and the rest of a section (UINT32_MAX for none) not built
    // or being built, next behind the game or, where nothing is built behind it, in the round too. Called with
    // prebuildMutex held.
    void RasterShaderCache::startPrebuildRound(uint32_t section, std::unique_lock<std::mutex> &lock) {
#   if defined(RT64_PREBUILT_SHADERS)
        const bool dualSource = device->getCapabilities().dualSourceBlending;
        prebuildRound.clear();
        if (section < RT64PrebuiltShaderSectionCount) {
            prebuildSection = section;
            const std::vector<uint32_t> &indices = prebuildSections[section];
            for (auto it = indices.rbegin(); it != indices.rend(); it++) {
                if (prebuildState[*it] == PrebuildTaken) {
                    continue;
                }
                else if (specializedShaders) {
                    prebuildAhead.emplace_front(*it);
                }
                else {
                    prebuildState[*it] = PrebuildTaken;
                    prebuildRound.emplace_back(*it);
                }
            }
        }

        for (uint32_t index : prebuildWanted) {
            if (prebuildState[index] == PrebuildWanted) {
                prebuildState[index] = PrebuildTaken;
                prebuildRound.emplace_back(index);
            }
        }

        prebuildWanted.clear();
        std::stable_sort(prebuildRound.begin(), prebuildRound.end(), [dualSource](uint32_t a, uint32_t b) {
            return roundSize(a, dualSource) > roundSize(b, dualSource);
        });

        prebuildRoundNext = 0;
        prebuildRoundDone = 0;
        prebuildRoundSize = 0;
        prebuildRoundSizeDone = 0;
        prebuildRoundSizeStarted = 0;
        for (uint32_t index : prebuildRound) {
            prebuildRoundSize += roundSize(index, dualSource);
        }

        // Section 0's is reported once the game draws (getGPUShader()): nothing the launcher draws uses it.
        prebuildRoundHeld = !prebuildRound.empty();
        if (prebuildRoundHeld && prebuildDrawn) {
            prebuildTimer.reset();
            printf("RT64: building %u prebuilt shaders for section %s, the game waiting\n", uint32_t(prebuildRound.size()),
                (section < RT64PrebuiltShaderSectionCount) ? RT64PrebuiltShaderSections[section].name : "(none)");
            reportPrebuild(0, uint32_t(prebuildRound.size()), 0, 0, prebuildRoundSize);
        }

        prebuildChanged.notify_all();
#   endif
    }

    void RasterShaderCache::reportPrebuild(uint32_t done, uint32_t count, uint64_t sizeDone, uint64_t sizeStarted, uint64_t size) {
#   if defined(__EMSCRIPTEN__)
        const double fraction = (size > 0) ? double(sizeDone + sizeStarted) / double(2 * size) : 1.0;
        MAIN_THREAD_ASYNC_EM_ASM({ if (Module['rt64ShaderProgress']) Module['rt64ShaderProgress']($0, $1, $2); }, done, count, fraction);
#   endif
    }

    // Every thread builds the round's shaders, and between rounds the first one builds ahead of the game, with up to
    // half of them while what is next is section 0's or 1's (startPrebuild()).
    void RasterShaderCache::prebuildLoop(uint32_t thread) {
#   if defined(RT64_PREBUILT_SHADERS)
        Thread::setCurrentThreadName("RT64 Prebuild");
        const bool dualSource = device->getCapabilities().dualSourceBlending;
        std::unique_lock<std::mutex> lock(prebuildMutex);
        while (true) {
            prebuildChanged.wait(lock, [this, thread]() {
                return prebuildStop || (prebuildRoundNext < prebuildRound.size()) ||
                    (!prebuildAhead.empty() && ((thread == 0) || ((thread < prebuildBootThreads) && prebuildBoot[prebuildAhead.front()])));
            });

            if (prebuildStop) {
                break;
            }

            const bool inRound = (prebuildRoundNext < prebuildRound.size());
            uint32_t index;
            if (inRound) {
                index = prebuildRound[prebuildRoundNext++];
                prebuildRoundSizeStarted += roundSize(index, dualSource);
            }
            else {
                index = prebuildAhead.front();
                prebuildAhead.pop_front();
                if (prebuildState[index] == PrebuildTaken) {
                    continue;
                }

                prebuildState[index] = PrebuildTaken;
            }

            lock.unlock();

            ShaderDescription desc = (index < RT64PrebuiltShaderCount) ? prebuiltDescription(RT64PrebuiltShaderList[index]) : prebuildLearned[index - RT64PrebuiltShaderCount];
            std::unique_ptr<RasterShader> shader;
            const bool decalStencil = (RenderTarget::depthBufferFormat() == RenderFormat::D32_FLOAT_S8_UINT);
            if ((index < RT64PrebuiltShaderCount) && (decalStencil || !RasterShader::prebuiltDecalStencil(desc))) {
                const RT64PrebuiltShader &entry = RT64PrebuiltShaderList[index];
                const char *text = prebuildText.data();
                const uint32_t pixelOffset = dualSource ? entry.pixelOffset : entry.pixelSingleOffset;
                shader = std::make_unique<RasterShader>(device, desc, shaderUber->pipelineLayout.get(), multisampling,
                    text + entry.vertexOffset, entry.vertexSize, text + pixelOffset, prebuiltPixelSize(entry, dualSource));
            }
            else {
                shader = std::make_unique<RasterShader>(device, desc, shaderUber->pipelineLayout.get(), shaderFormat, multisampling,
                    shaderCompiler.get(), &optimizerCacheSPIRV, &shaderCacheWGSL);
            }

            {
                const std::shared_ptr<RasterShader> built = std::move(shader);
                const std::unique_lock<std::mutex> shadersLock(GPUShadersMutex);
                GPUShaders[desc.hash()] = built;
                if (index < RT64PrebuiltShaderCount) {
                    for (uint64_t alias : prebuildAliases[index]) {
                        GPUShaders[alias] = built;
                    }
                }
            }

            lock.lock();
            if (inRound) {
                prebuildRoundDone++;
                prebuildRoundSizeDone += roundSize(index, dualSource);
                reportPrebuild(prebuildRoundDone, uint32_t(prebuildRound.size()), prebuildRoundSizeDone, prebuildRoundSizeStarted, prebuildRoundSize);
                if (prebuildRoundDone == prebuildRound.size()) {
                    prebuildRoundHeld = false;
                    printf("RT64: %u prebuilt shaders built in %.1f s, the game waiting\n", uint32_t(prebuildRound.size()), prebuildTimer.elapsedSeconds());
                }
            }

            if (++prebuildBuilt == prebuildCount) {
                prebuildText = std::string();
                printf("RT64: all %u prebuilt shaders built\n", prebuildCount);
            }

            prebuildChanged.notify_all();
        }
#   endif
    }

    // Before each frame: holds it until the round is built, and then, if the last frame wanted prebuilt shaders,
    // decides what builds them (startPrebuild() says how). Returns whether the game was held.
    bool RasterShaderCache::waitForPrebuild() {
        bool held = false;
#   if defined(RT64_PREBUILT_SHADERS)
        // Until something has drawn, no WebGPU layout has been resolved for the prebuilt shaders to be built for.
        if (!prebuildDrawn) {
            return false;
        }

        std::unique_lock<std::mutex> lock(prebuildMutex);
        while (true) {
            held = held || prebuildRoundHeld;
            prebuildChanged.wait(lock, [this]() { return !prebuildRoundHeld; });
            if (prebuildWanted.empty()) {
                break;
            }

            uint32_t bestSection = UINT32_MAX;
            uint32_t bestCount = 0;
            for (uint32_t s = 0; s < RT64PrebuiltShaderSectionCount; s++) {
                uint32_t count = 0;
                for (uint32_t index : prebuildSections[s]) {
                    count += (prebuildState[index] == PrebuildWanted) ? 1 : 0;
                }

                if (count > bestCount) {
                    bestSection = s;
                    bestCount = count;
                }
            }

            if (bestCount >= PrebuildSectionMinimum) {
                startPrebuildRound(bestSection, lock);
            }
            else if (!specializedShaders) {
                startPrebuildRound(UINT32_MAX, lock);
            }
            else {
                for (uint32_t index : prebuildWanted) {
                    if (prebuildState[index] == PrebuildWanted) {
                        prebuildState[index] = PrebuildSoon;
                        prebuildAhead.emplace_front(index);
                    }
                }

                prebuildWanted.clear();
                prebuildChanged.notify_all();
            }
        }

        lock.unlock();
        held = buildMissed() || held;
#   endif
        return held;
    }

    // A draw asked for a prebuilt shader that isn't built yet.
    void RasterShaderCache::wantPrebuild(uint64_t shaderHash) {
        std::unique_lock<std::mutex> lock(prebuildMutex);
        auto indexIt = prebuildIndices.find(shaderHash);
        if ((indexIt != prebuildIndices.end()) && (prebuildState[indexIt->second] == PrebuildQueued)) {
            prebuildState[indexIt->second] = PrebuildWanted;
            prebuildWanted.emplace_back(indexIt->second);
        }
    }

    void RasterShaderCache::submit(const ShaderDescription &desc) {
        // A browser that builds no shaders behind the game (Firefox) still learns the ones the list missed, builds them
        // before the next frame (buildMissed()), and with their section on the next visit.
        if (!specializedShaders && !recordShaders && (prebuildCount == 0)) {
            return;
        }

        {
            std::unique_lock<std::mutex> queueLock(submissionMutex);

            // Verify if an entry with the same hash was already submitted before.
            const uint64_t shaderHash = desc.hash();
            bool &found = shaderHashes[shaderHash];
            if (found) {
                return;
            }

            found = true;
        }

        // The spec constants' order: other mode L and H, color combiner L and H, flags.
        if (recordShaders) {
            printf("rt64 shader: %08x %08x %08x %08x %08x\n", desc.otherMode.L, desc.otherMode.H, desc.colorCombiner.L, desc.colorCombiner.H, desc.flags.value);
        }

#   if defined(RT64_PREBUILT_SHADERS)
        // What the list has is marked submitted before anything draws (startPrebuild()), so the list missed this one.
        if (prebuildCount > 0) {
            reportMissed(desc);
        }
#   endif

        if (!specializedShaders) {
#       if defined(RT64_PREBUILT_SHADERS)
            if (prebuildCount > 0) {
                const std::unique_lock<std::mutex> missedLock(missedMutex);
                missedQueue.emplace_back(desc);
            }
#       endif
            return;
        }

        // Push a new shader compilation to the queue.
        {
            const std::unique_lock<std::mutex> queueLock(descQueueMutex);
            descQueue.push(desc);
        }

        descQueueChanged.notify_all();
    }
    
    // Where nothing is built during play (Firefox, setup()), a shader the list missed is built before the next frame,
    // the game waiting, as a stray of the list is (startPrebuild()). Drawn with the ubershader instead, it stayed so
    // for the rest of the visit: waiting for a moment when the game sends no frames found none in most of a long play,
    // and the misses come where the list's sections have all been reached, with no section's wait after them. Each
    // is built from the ubershader's WGSL with its spec constants as overrides, as a learned one is. Called by
    // waitForPrebuild(), on the workload thread; returns whether the game waited.
    bool RasterShaderCache::buildMissed() {
        std::deque<ShaderDescription> missed;
        {
            const std::unique_lock<std::mutex> missedLock(missedMutex);
            missed.swap(missedQueue);
        }

        if (missed.empty()) {
            return false;
        }

        ElapsedTimer timer;
        const uint32_t count = uint32_t(missed.size());
        reportPrebuild(0, count, 0, 0, count);
        for (uint32_t i = 0; i < count; i++) {
            const ShaderDescription &desc = missed[i];
            const std::shared_ptr<RasterShader> shader = std::make_shared<RasterShader>(device, desc, shaderUber->pipelineLayout.get(), shaderFormat,
                multisampling, shaderCompiler.get(), &optimizerCacheSPIRV, &shaderCacheWGSL);
            {
                const std::unique_lock<std::mutex> shadersLock(GPUShadersMutex);
                GPUShaders[desc.hash()] = shader;
            }

            reportPrebuild(i + 1, count, i + 1, i + 1, count);
        }

        printf("RT64: %u shaders the list missed built in %.1f s, the game waiting\n", count, timer.elapsedSeconds());
        return true;
    }

    void RasterShaderCache::waitForAll() {
        {
            std::unique_lock<std::mutex> queueLock(descQueueMutex);
            descQueue = std::queue<ShaderDescription>();
        }

        bool keepWaiting = false;
        do {
            std::unique_lock<std::mutex> queueLock(descQueueMutex);
            keepWaiting = (descQueueActiveCount > 0);
        } while (keepWaiting);
    }

    void RasterShaderCache::destroyAll() {
        {
            std::unique_lock<std::mutex> lock(GPUShadersMutex);
            GPUShaders.clear();
        }

        {
            std::unique_lock<std::mutex> queueLock(submissionMutex);
            shaderHashes.clear();
        }
    }

    RasterShader *RasterShaderCache::getGPUShader(const ShaderDescription &desc) {
        const uint64_t shaderHash = desc.hash();

        const std::unique_lock<std::mutex> lock(GPUShadersMutex);
        auto shaderIt = GPUShaders.find(shaderHash);
        if (shaderIt == GPUShaders.end()) {
            // The first draw starts the prebuilt shaders' builds (startPrebuild()), and the page shows section 0's
            // progress from there: nothing the launcher draws uses them.
            if ((prebuildCount > 0) && !prebuildDrawn.exchange(true)) {
                std::unique_lock<std::mutex> prebuildLock(prebuildMutex);
                prebuildTimer.reset();
                reportPrebuild(prebuildRoundDone, uint32_t(prebuildRound.size()), prebuildRoundSizeDone, prebuildRoundSizeStarted, prebuildRoundSize);
            }

            if (prebuildBuilt < prebuildCount) {
                wantPrebuild(shaderHash);
            }

            return nullptr;
        }

        return shaderIt->second.get();
    }

    RasterShaderUber *RasterShaderCache::getGPUShaderUber() const {
        return shaderUber.get();
    }

    uint32_t RasterShaderCache::shaderCount() {
        std::unique_lock<std::mutex> lock(GPUShadersMutex);
        return GPUShaders.size();
    }
};