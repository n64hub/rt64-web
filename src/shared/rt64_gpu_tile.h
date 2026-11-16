//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
#ifdef HLSL_CPU
    union GPUTileFlags {
        struct {
            uint alphaIsCvg : 1;
            uint highRes : 1;
            uint fromCopy : 1;
            uint rawTMEM : 1;
            uint hasMipmaps : 1;
            uint shiftedByHalf : 1;
        };

        uint value;
    };
#else
    // SPIR-V code generation does not seem to like bitfields at the moment, so we work around it by querying the flags manually.
    typedef uint GPUTileFlags;

    bool gpuTileFlagAlphaIsCvg(GPUTileFlags flags) {
        return flags & 0x1;
    }

    bool gpuTileFlagHighRes(GPUTileFlags flags) {
        return flags & 0x2;
    }

    bool gpuTileFlagFromCopy(GPUTileFlags flags) {
        return flags & 0x4;
    }

    bool gpuTileFlagRawTMEM(GPUTileFlags flags) {
        return flags & 0x8;
    }

    bool gpuTileFlagHasMipmaps(GPUTileFlags flags) {
        return flags & 0x10;
    }

    bool gpuTileFlagShiftedByHalf(GPUTileFlags flags) {
        return flags & 0x20;
    }
#endif
    struct GPUTile {
        float2 ulScale;
        float2 tcScale;
        uint2 texelShift;
        uint2 texelMask;
        // textureDimensions starts on 16 bytes and the struct is a multiple of 16 long, so the D3D packing dxc gives a
        // StructuredBuffer (-fvk-use-dx-layout) is also the layout WGSL gives a storage buffer (vec3 aligns to 16).
        float3 textureDimensions;
        uint textureIndex;
        GPUTileFlags flags;
        // Set by the shader in its copy, never on the CPU: the tile's slot in its draw's texture set (PER_DRAW_TEXTURES,
        // TextureSampler.hlsli).
        uint drawSlot;
        uint pad[2];
    };
#ifdef HLSL_CPU
    static_assert(sizeof(GPUTile) == 64, "GPUTile must keep the layout WGSL gives it.");
#endif
#ifdef HLSL_CPU
};
#endif