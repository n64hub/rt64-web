//
// RT64
//

#pragma once

#include "rt64_shader_common.h"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "re-spirv/re-spirv.h"

#include "plume_render_interface.h"
#include "shared/rt64_blender.h"
#include "shared/rt64_color_combiner.h"
#include "shared/rt64_other_mode.h"

#include "rt64_shader_library.h"
#include "rt64_shader_compiler.h"

// Require the use of sample locations for MSAA.
#define SAMPLE_LOCATIONS_REQUIRED 1

namespace RT64 {
    struct OptimizerCacheSPIRV {
        respv::Shader rasterVS;
        respv::Shader rasterVSFlat;
        respv::Shader rasterPS;
        respv::Shader rasterPSMS;
        respv::Shader rasterPSDepth;
        respv::Shader rasterPSDepthMS;
        respv::Shader rasterPSFlatDepth;
        respv::Shader rasterPSFlatDepthMS;
        respv::Shader rasterPSFlat;
        respv::Shader rasterPSFlatMS;

        void initialize(const RenderDeviceCapabilities &capabilities);
    };

    // The spec constant shaders a WGSL build specializes with override values, each created once: they are the same
    // source for every shader, and creating a RasterPS module (250 KB of WGSL) costs the browser a parse each time.
    struct ShaderCacheWGSL {
        std::mutex mutex;
        std::unordered_map<const void *, std::unique_ptr<RenderShader>> shaders;

        // The vertex and pixel shader every shader specializes, created on first use.
        void get(RenderDevice *device, bool useMSAA, const RenderShader *&vertexShader, const RenderShader *&pixelShader);
        const RenderShader *get(RenderDevice *device, const void *blob, uint64_t size, const char *entryPointName);
    };

    // A prebuilt decal's two draws on WebGPU (RasterShader::createPipeline()).
    enum class DecalStencil {
        None,
        Mark,
        Draw
    };

    struct PipelineCreation {
        RenderDevice *device;
        const RenderPipelineLayout *pipelineLayout;
        const RenderShader *vertexShader;
        const RenderShader *pixelShader;
        bool alphaBlend;
        bool culling;
        bool NoN;
        bool zCmp;
        bool zUpd;
        bool cvgAdd;
        bool usesHDR;
        std::vector<RenderSpecConstant> specConstants;
        RenderMultisampling multisampling;
        bool buildInBackground = false;
        DecalStencil decalStencil = DecalStencil::None;
    };

    struct RasterShaderText {
        std::string vertexShader;
        std::string pixelShader;
    };

    struct RasterShader {
        ShaderDescription desc;
        RenderDevice *device;
        std::unique_ptr<RenderPipeline> pipeline;

        // A prebuilt decal's pixel shader writes the bounds of the decal check as its depth (RasterPS.hlsl,
        // DECAL_DEPTH_BOUNDS), and the decal draws twice in the pass that writes depth: with this pipeline and then
        // with the one above (createPipeline()). Null for every other shader.
        std::unique_ptr<RenderPipeline> decalMarkPipeline;

        RasterShader(RenderDevice *device, const ShaderDescription &desc, const RenderPipelineLayout *pipelineLayout, RenderShaderFormat shaderFormat, const RenderMultisampling &multisampling, 
            const ShaderCompiler *shaderCompiler, const OptimizerCacheSPIRV *optimizerCacheSPIRV, ShaderCacheWGSL *shaderCacheWGSL);

        // A shader specialized when the program was built (RT64_PREBUILT_SHADER_LIST in CMakeLists.txt): WGSL with the
        // description's spec constants folded in, so the browser builds it from far less code.
        RasterShader(RenderDevice *device, const ShaderDescription &desc, const RenderPipelineLayout *pipelineLayout, const RenderMultisampling &multisampling,
            const char *vertexWGSL, size_t vertexWGSLSize, const char *pixelWGSL, size_t pixelWGSLSize);

        ~RasterShader();
        void createPipelineFor(const RenderPipelineLayout *pipelineLayout, const RenderShader *vertexShader, const RenderShader *pixelShader, const std::vector<RenderSpecConstant> &specConstants, const RenderMultisampling &multisampling, bool decalStencil = false);
        static bool prebuiltDecalStencil(const ShaderDescription &desc);
        static void setPipelineState(const ShaderDescription &desc, PipelineCreation &creation);
        static uint32_t pipelineStateKey(const ShaderDescription &desc);
        static RasterShaderText generateShaderText(const ShaderDescription &desc, bool multisampling);
        static std::unique_ptr<RenderPipeline> createPipeline(const PipelineCreation &c);
        static RenderMultisampling generateMultisamplingPattern(uint32_t sampleCount, bool sampleLocationsSupported);
    };

    struct RasterShaderUber {
        static const uint64_t RasterVSLibraryHash;
        static const uint64_t RasterPSLibraryHash;

        std::unique_ptr<RenderPipeline> pipelines[8];
        std::unique_ptr<RenderPipeline> postBlendDitherNoiseAddPipeline;
        std::unique_ptr<RenderPipeline> postBlendDitherNoiseSubPipeline;
        std::unique_ptr<RenderPipeline> postBlendDitherNoiseSubNegativePipeline;
        std::mutex firstPipelineMutex;
        std::condition_variable firstPipelineCondition;
        bool pipelinesCreated = false;
        std::unique_ptr<RenderPipelineLayout> pipelineLayout;
        std::vector<std::vector<PipelineCreation>> pipelineThreadCreations;
        std::vector<std::unique_ptr<std::thread>> pipelineThreads;
        std::unique_ptr<RenderShader> vertexShader;
        std::unique_ptr<RenderShader> pixelShader;

        RasterShaderUber(RenderDevice *device, RenderShaderFormat shaderFormat, const RenderMultisampling &multisampling, const ShaderLibrary *shaderLibrary, uint32_t threadCount);
        ~RasterShaderUber();
        void threadCreatePipelines(uint32_t threadIndex);
        void waitForPipelineCreation();
        uint32_t pipelineStateIndex(bool zCmp, bool zUpd, bool cvgAdd) const;
        const RenderPipeline *getPipeline(bool zCmp, bool zUpd, bool cvgAdd) const;
    };
};
