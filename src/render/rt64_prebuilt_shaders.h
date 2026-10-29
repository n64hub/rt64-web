//
// RT64
//

#pragma once

#include <stdint.h>

// The shaders a game prebuilds on WebGPU (RT64_PREBUILT_SHADER_LIST in CMakeLists.txt, RasterShaderCache::startPrebuild()):
// a shader description each, and where its vertex and pixel shaders' WGSL is in the text that RT64PrebuiltShaderData
// decompresses to (zstd), zero-terminated, the sizes counting the zero: the pixel shader with dual-source blending, and
// the one for a device without it. shader_specializer writes them, each description once. The list is in sections,
// a game's screens or levels: section 0 is built before the game gets past its first frame, and another when the
// game first draws with its shaders (RasterShaderCache::startPrebuild()). A section is a range of
// RT64PrebuiltShaderSectionEntries, which index RT64PrebuiltShaderList; a shader can be in several.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t otherModeL;
    uint32_t otherModeH;
    uint32_t combinerL;
    uint32_t combinerH;
    uint32_t flags;
    uint32_t vertexOffset;
    uint32_t vertexSize;
    uint32_t pixelOffset;
    uint32_t pixelSize;
    uint32_t pixelSingleOffset;
    uint32_t pixelSingleSize;
} RT64PrebuiltShader;

typedef struct {
    const char *name;
    uint32_t first;
    uint32_t count;
} RT64PrebuiltShaderSection;

extern const RT64PrebuiltShader RT64PrebuiltShaderList[];
extern const uint32_t RT64PrebuiltShaderCount;
extern const RT64PrebuiltShaderSection RT64PrebuiltShaderSections[];
extern const uint32_t RT64PrebuiltShaderSectionCount;
extern const uint32_t RT64PrebuiltShaderSectionEntries[];
extern const uint32_t RT64PrebuiltShaderTextSize;
extern const uint32_t RT64PrebuiltShaderDataSize;
extern const unsigned char RT64PrebuiltShaderData[];

#ifdef __cplusplus
}
#endif
