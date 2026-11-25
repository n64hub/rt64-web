//
// RT64
//

#pragma once

#include "shared/rt64_hlsl.h"

#ifdef HLSL_CPU
namespace interop {
#endif
    struct RenderIndices {
        uint instanceIndex;
        uint faceIndicesStart;
        uint rdpTileIndex;
        uint rdpTileCount;
        uint highlightColor;
        // RT64_RECT_OWN_PIXEL_GRID: a texrect's texcoords at its first pixel and their steps a native pixel, its dsdx and
        // dtdy in texels; steps of 0 for every other draw, and in every build without the option, whose shaders don't
        // read them (RasterPS.hlsl). In every build all the same, so that a shader compiled without the option reads the
        // block at its stride.
#ifdef HLSL_CPU
        float rectGridU = 0.0f;
        float rectGridV = 0.0f;
        float rectGridStepU = 0.0f;
        float rectGridStepV = 0.0f;
#else
        float rectGridU;
        float rectGridV;
        float rectGridStepU;
        float rectGridStepV;
#endif
        // RT64_UPSCALED_RECT_LAST_TEXEL: the box of texcoords a texrect drawn upscaled samples, from its first pixel's to
        // its last's on the RDP (RasterPS.hlsl); a max below its min for every other draw, and in every build without
        // the option, whose shaders don't read it. In every build all the same, as the grid's are.
#ifdef HLSL_CPU
        float rectTexelMinU = 0.0f;
        float rectTexelMinV = 0.0f;
        float rectTexelMaxU = -1.0f;
        float rectTexelMaxV = -1.0f;
#else
        float rectTexelMinU;
        float rectTexelMinV;
        float rectTexelMaxU;
        float rectTexelMaxV;
#endif
    };
#ifdef HLSL_CPU
};
#endif