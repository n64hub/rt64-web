//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

#define RSP_LOOKAT_INDEX_ENABLED 0x1
#define RSP_LOOKAT_INDEX_LINEAR 0x2
#define RSP_LOOKAT_INDEX_SHIFT 2

#ifdef HLSL_CPU
namespace interop {
#endif
    struct RSPLookAt {
        // Each float3 starts on 16 bytes and the struct is 32 long, so the D3D packing dxc gives a StructuredBuffer
        // (-fvk-use-dx-layout) is also the layout WGSL gives a storage buffer (vec3 aligns to 16).
        float3 x;
        float xPad;
        float3 y;
        float yPad;
    };
#ifdef HLSL_CPU
    static_assert(sizeof(RSPLookAt) == 32, "RSPLookAt must keep the layout WGSL gives it.");
#endif
#ifdef HLSL_CPU
};
#endif