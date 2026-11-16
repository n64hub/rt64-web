//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
    struct VideoInterfaceCB {
        float2 upperLeft; // where the area the VI shows starts in the texture: below the rows before its origin
        float2 videoResolution;
        float2 textureResolution;
        float gamma;
    };
#ifdef HLSL_CPU
};
#endif