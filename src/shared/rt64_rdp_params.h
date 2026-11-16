//
// RT64
//

#pragma once

#include "rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
    struct RDPParams {
        float4 primColor;
        float2 primLOD;
        float2 primDepth;
        float4 envColor;
        float4 fogColor;
        float4 blendColor;
        // Every float3 starts on 16 bytes and the struct is a multiple of 16 long, so the D3D packing dxc gives a
        // StructuredBuffer (-fvk-use-dx-layout) is also the layout WGSL gives a storage buffer (vec3 aligns to 16).
        float3 keyCenter;
        float keyCenterPad;
        float3 keyScale;
        float keyScalePad;
        int convertK[6];
        int convertKPad[2];
    };
#ifdef HLSL_CPU
    static_assert(sizeof(RDPParams) == 144, "RDPParams must keep the layout WGSL gives it.");
#endif
#ifdef HLSL_CPU
};
#endif