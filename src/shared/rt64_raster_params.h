//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

// A WGSL build gives each raster draw its index into instanceRenderIndices as the draw's first instance, which RasterVS
// reads as SV_InstanceID (WGSL's instance_index counts from it) and hands the pixel shaders as a flat varying
// (RasterRenderIndex.hlsli), and leaves renderIndex 0: the push constants are then the same block draw after draw, which
// plume's WebGPU backend binds once in a pass, where a block a draw was a slot of its ring and a setBindGroup on the
// page's thread each. Defined here, beside the block, so that the renderer and the shaders follow the same header: a
// build with its own copy of this file draws by renderIndex in both.
#if (defined(HLSL_CPU) && defined(RT64_WGSL)) || (!defined(HLSL_CPU) && defined(WEBGPU))
#   define RT64_RENDER_INDEX_BY_INSTANCE
#endif

#ifdef HLSL_CPU
namespace interop {
#endif
    struct RasterParams {
        uint renderIndex;
        // Three words, not a uint3: WGSL starts a vec3 on 16 bytes, which would move what follows it.
        uint padding0;
        uint padding1;
        uint padding2;
        float2 screenScale;
        float2 screenOffset;
    };
#ifdef HLSL_CPU
    static_assert(sizeof(RasterParams) == 32, "RasterParams must keep the layout WGSL gives it.");
#endif
#ifdef HLSL_CPU
};
#endif