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
        // RT64_RECT_OWN_PIXEL_GRID: a texrect's native pixel width in 256ths of a pixel (the low 16 bits), then where its
        // left and top edges fall in its native pixel, in 256ths of its width and height (the next 8 bits and the top 8);
        // 0 for every other draw, and in every build without the option, whose shaders don't read it (RasterPS.hlsl).
        // In every build all the same, so that a shader compiled without the option reads the block at its stride.
#ifdef HLSL_CPU
        uint rectPixelGrid = 0;
#else
        uint rectPixelGrid;
#endif
    };
#ifdef HLSL_CPU
};
#endif