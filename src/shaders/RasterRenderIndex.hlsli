//
// RT64
//

#pragma once

// The draw's index into instanceRenderIndices: the push constants' renderIndex, or with RT64_RENDER_INDEX_BY_INSTANCE
// (rt64_raster_params.h) the draw's first instance, which each entry point sets gRenderIndex to.
#ifdef RT64_RENDER_INDEX_BY_INSTANCE
static uint gRenderIndex;
#   define RENDER_INDEX gRenderIndex
#else
#   define RENDER_INDEX gConstants.renderIndex
#endif
