//
// RT64
//

#pragma once

#include "FbRendererCommon.hlsli"
#include "Math.hlsli"
#include "TextureDecoder.hlsli"

#include "shared/rt64_other_mode.h"
#include "shared/rt64_render_flags.h"
#include "shared/rt64_render_params.h"

#define SIMULATE_LOW_PRECISION 1
#define FIX_UPSCALING_PRECISION 1

// We prefer using for loops to reduce the size of the final ubershader as much as possible.
// When using specialized shaders, we prefer to unroll the loop manually to give re-spirv an
// easier time to eliminate dead code, as its graph model can't handle the loop constructs yet.
// A WGSL build's spec constant shaders, which the browser specializes with override values and nothing strips of dead
// code beforehand, keep the loops too (SPEC_CONSTANT_LOOPS, CMakeLists.txt).
#if defined(DYNAMIC_RENDER_PARAMS) || defined(SPEC_CONSTANT_LOOPS)
#define USE_FOR_LOOPS 1
#else
#define USE_FOR_LOOPS 0
#endif

// Where a tile's texture is. With descriptor indexing, textureIndex is the texture cache index of the tile's texture
// (set 1) or of its TMEM (set 2). Without it (PER_DRAW_TEXTURES), the renderer binds each draw's textures in a set of
// their own, one slot per tile of the draw, so a texture is picked by the tile's number within its draw, which
// drawGPUTile() puts in drawSlot. The cache index still finds the TMEM, in gTMEMPool. A switch picks the slot, since
// core WebGPU can't index bindings. The slot is never read from a buffer, so a specialized shader that knows the
// tile's number (one without mip LOD) knows its slot, and spirv-opt keeps only that case. In the bits of textureIndex,
// every texture read stayed a switch of 8 reads: a prebuilt pixel shader without LOD is a third of the WGSL without
// it, and in Firefox 158, which builds pipelines one at a time, Majora's Mask's Clock Town took 9.3-10.4 s against
// 15.1-18.2 s and Rush 2049's boot 11.9-12.6 s against 13.6-14.6 s (RTX 2070). Shaders with LOD keep the switch.
#ifdef PER_DRAW_TEXTURES
#define tileTMEM(gpuTile) ((gpuTile).textureIndex * RDP_TMEM_BYTES)

#define TILE_TEXTURE_SWITCH(gpuTile, TEXTURE_CALL) \
    switch ((gpuTile).drawSlot) { \
    case 0: return TEXTURE_CALL(gTileTexture0); \
    case 1: return TEXTURE_CALL(gTileTexture1); \
    case 2: return TEXTURE_CALL(gTileTexture2); \
    case 3: return TEXTURE_CALL(gTileTexture3); \
    case 4: return TEXTURE_CALL(gTileTexture4); \
    case 5: return TEXTURE_CALL(gTileTexture5); \
    case 6: return TEXTURE_CALL(gTileTexture6); \
    default: return TEXTURE_CALL(gTileTexture7); \
    }

// The same with the slot's sampler (gTileSampler0 to 7), for a linear sample.
#define TILE_TEXTURE_SAMPLER_SWITCH(gpuTile, TEXTURE_SAMPLER_CALL) \
    switch ((gpuTile).drawSlot) { \
    case 0: return TEXTURE_SAMPLER_CALL(gTileTexture0, gTileSampler0); \
    case 1: return TEXTURE_SAMPLER_CALL(gTileTexture1, gTileSampler1); \
    case 2: return TEXTURE_SAMPLER_CALL(gTileTexture2, gTileSampler2); \
    case 3: return TEXTURE_SAMPLER_CALL(gTileTexture3, gTileSampler3); \
    case 4: return TEXTURE_SAMPLER_CALL(gTileTexture4, gTileSampler4); \
    case 5: return TEXTURE_SAMPLER_CALL(gTileTexture5, gTileSampler5); \
    case 6: return TEXTURE_SAMPLER_CALL(gTileTexture6, gTileSampler6); \
    default: return TEXTURE_SAMPLER_CALL(gTileTexture7, gTileSampler7); \
    }
#else
#define tileTMEM(gpuTile) gTMEM[NonUniformResourceIndex((gpuTile).textureIndex)]
#define TILE_TEXTURE_SWITCH(gpuTile, TEXTURE_CALL) return TEXTURE_CALL(gTextures[NonUniformResourceIndex((gpuTile).textureIndex)]);
#endif

// The GPU tile of the draw's tile tileIndex (0 to 7).
GPUTile drawGPUTile(uint rdpTileIndex, uint tileIndex) {
    GPUTile gpuTile = GPUTiles[rdpTileIndex + tileIndex];
#ifdef PER_DRAW_TEXTURES
    gpuTile.drawSlot = tileIndex;
#endif
    return gpuTile;
}

float4 loadTileTexture(const GPUTile gpuTile, int3 texelMip) {
#define TEXTURE_CALL(texture) texture.Load(texelMip)
    TILE_TEXTURE_SWITCH(gpuTile, TEXTURE_CALL)
#undef TEXTURE_CALL
}

#ifdef PER_DRAW_TEXTURES
// A texel for each tap of a filter (only the first without filtering), in one switch over the slots.
#define LOAD_TILE_TEXELS(texture) \
    samples[0] = texture.Load(int3(texels[0], mipLevel)); \
    if (filtering) { \
        samples[1] = texture.Load(int3(texels[1], mipLevel)); \
        samples[2] = texture.Load(int3(texels[2], mipLevel)); \
        samples[3] = texture.Load(int3(texels[3], mipLevel)); \
    } \
    break;

void loadTileTexels(const GPUTile gpuTile, int2 texels[4], uint mipLevel, bool filtering, inout float4 samples[4]) {
    switch (gpuTile.drawSlot) {
    case 0: LOAD_TILE_TEXELS(gTileTexture0)
    case 1: LOAD_TILE_TEXELS(gTileTexture1)
    case 2: LOAD_TILE_TEXELS(gTileTexture2)
    case 3: LOAD_TILE_TEXELS(gTileTexture3)
    case 4: LOAD_TILE_TEXELS(gTileTexture4)
    case 5: LOAD_TILE_TEXELS(gTileTexture5)
    case 6: LOAD_TILE_TEXELS(gTileTexture6)
    default: LOAD_TILE_TEXELS(gTileTexture7)
    }
}
#undef LOAD_TILE_TEXELS
#endif

void computeLOD(OtherMode otherMode, uint rdpTileCount, float2 primLOD, float resLodScale, float2 ddxUV, float2 ddyUV, inout int tileIndex0, inout int tileIndex1, out float lodFraction) {
    const bool usesLOD = (otherMode.textLOD() == G_TL_LOD);
    if (usesLOD) {
        const bool lodSharpen = (otherMode.textDetail() & G_TD_SHARPEN) != 0;
        const bool lodDetail = (otherMode.textDetail() & G_TD_DETAIL) != 0;
        const int tileMax = int(rdpTileCount) - 1;
        float2 maxDdUV = max(abs(ddxUV), abs(ddyUV));
        float maxDst = max(maxDdUV.x, maxDdUV.y) * resLodScale;
        if (lodDetail || lodSharpen) {
            maxDst = max(maxDst, primLOD.y);
        }

        int tileBase = floor(log2(maxDst));
        lodFraction = maxDst / pow(2, max(tileBase, 0)) - 1.0f;

        if (lodSharpen) {
            if (maxDst < 1.0f)
                lodFraction = maxDst - 1.0f;
        }

        if (lodDetail) {
            if (lodFraction < 0.0f)
                lodFraction = maxDst;
            tileBase += 1;
        }
        else {
            if (tileBase >= tileMax) {
                lodFraction = 1.0f;
            }
        }

        if (lodDetail || lodSharpen) {
            tileBase = max(tileBase, 0);
        }
        else {
            lodFraction = max(lodFraction, 0.0f);
        }

        tileIndex0 = clamp(tileBase, 0, tileMax);
        tileIndex1 = clamp(tileBase + 1, 0, tileMax);
    }
    else {
        lodFraction = 1.0f;

    }
}

// The texel a tap reads, with the tile's clamp, wrap and mirror applied.
int2 clampWrapMirrorTexel(const RDPTile rdpTile, const GPUTile gpuTile, float2 tcScale, int2 texelInt) {
    if (rdpTile.cms & G_TX_CLAMP) {
        texelInt.x = clamp(texelInt.x, 0, (round(tcScale.x * rdpTile.lrs) / 4) - (round(tcScale.x * rdpTile.uls) / 4) + round(tcScale.x - 1.0f));
    }

    if (rdpTile.cmt & G_TX_CLAMP) {
        texelInt.y = clamp(texelInt.y, 0, (round(tcScale.y * rdpTile.lrt) / 4) - (round(tcScale.y * rdpTile.ult) / 4) + round(tcScale.y - 1.0f));
    }
    
    const int masks = round(rdpTile.masks * tcScale.x);
    if ((rdpTile.cms & G_TX_MIRROR) && (masks > 0) && (modulo(texelInt.x, masks * 2) >= masks)) {
        texelInt.x = (masks - 1) - modulo(texelInt.x, masks);
    }
    else {
        texelInt.x = modulo(texelInt.x, masks);
    }

    const int maskt = round(rdpTile.maskt * tcScale.y);
    if ((rdpTile.cmt & G_TX_MIRROR) && (maskt > 0) && (modulo(texelInt.y, maskt * 2) >= maskt)) {
        texelInt.y = (maskt - 1) - modulo(texelInt.y, maskt);
    }
    else {
        texelInt.y = modulo(texelInt.y, maskt);
    }
    
    // Allows selection of only particular columns or rows as defined by the tile.
    return (texelInt & gpuTile.texelMask) + gpuTile.texelShift;
}

float4 clampWrapMirrorSample(const RDPTile rdpTile, const GPUTile gpuTile, float2 tcScale, int2 texelInt, uint tlut, bool gpuTileUsesTMEM, uint mipLevel) {
    texelInt = clampWrapMirrorTexel(rdpTile, gpuTile, tcScale, texelInt);

    // Check if tile requires TMEM decoding and sample using dynamic decoding.
    if (gpuTileUsesTMEM) {
        return sampleTMEM(texelInt, rdpTile.siz, rdpTile.fmt, rdpTile.address, rdpTile.stride, tlut, rdpTile.palette, tileTMEM(gpuTile));
    }
    // Sample the color version directly.
    else {
        return loadTileTexture(gpuTile, int3(texelInt, mipLevel));
    }
}

static const float LowPrecision = 128.0f;

#ifdef WEBGPU
// WebGPU guarantees only 16 samplers in a stage, and nearest ones for every address mode would be 9 more. A nearest
// sample at a texel's centre is that texel, so it is loaded, with the sampler's address mode applied to the texel coordinate first: outside
// the switch over the draw's texture slots, which then holds only the load, rather than this once per slot.
int2 nativeSamplerTexel(uint nativeSampler, int2 texelBaseInt, float2 textureSize) {
    const int2 size = int2(textureSize);
    const uint addressModeU = (nativeSampler - 1) / 3;
    const uint addressModeV = (nativeSampler - 1) % 3;
    int2 texel = texelBaseInt;
    int2 period = int2(addressModeU == 1 ? 2 : 1, addressModeV == 1 ? 2 : 1) * size;
    int2 wrapped = ((texel % period) + period) % period;
    int2 mirrored = select(wrapped < size, wrapped, period - 1 - wrapped);
    texel.x = (addressModeU == 2) ? clamp(texel.x, 0, size.x - 1) : mirrored.x;
    texel.y = (addressModeV == 2) ? clamp(texel.y, 0, size.y - 1) : mirrored.y;
    return texel;
}
#else
float4 sampleTextureNative(Texture2D texture, uint nativeSampler, int2 texelBaseInt, float2 textureSize) {
    // Transform to native coordinate. Half pixel offset is required to reach the desired texel.
    float2 nativeUVCoord = (float2(texelBaseInt) + 0.5f) / textureSize;

    // Choose the native sampler that was determined to be compatible.
    switch (nativeSampler) {
        case NATIVE_SAMPLER_WRAP_WRAP:
            return texture.SampleLevel(gNearestWrapWrapSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_WRAP_MIRROR:
            return texture.SampleLevel(gNearestWrapMirrorSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_WRAP_CLAMP:
            return texture.SampleLevel(gNearestWrapClampSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_MIRROR_WRAP:
            return texture.SampleLevel(gNearestMirrorWrapSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_MIRROR_MIRROR:
            return texture.SampleLevel(gNearestMirrorMirrorSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_MIRROR_CLAMP:
            return texture.SampleLevel(gNearestMirrorClampSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_CLAMP_WRAP:
            return texture.SampleLevel(gNearestClampWrapSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_CLAMP_MIRROR:
            return texture.SampleLevel(gNearestClampMirrorSampler, nativeUVCoord, 0.0f);
        case NATIVE_SAMPLER_CLAMP_CLAMP:
        default:
            return texture.SampleLevel(gNearestClampClampSampler, nativeUVCoord, 0.0f);
    }
}
#endif

#ifndef PER_DRAW_TEXTURES
float4 sampleTextureGrad(Texture2D texture, uint nativeSampler, float2 nativeUVCoord, float2 ddxUVNorm, float2 ddyUVNorm) {
    // Choose the native sampler that was determined to be compatible.
    switch (nativeSampler) {
        case NATIVE_SAMPLER_WRAP_WRAP:
            return texture.SampleGrad(gLinearWrapWrapSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_WRAP_MIRROR:
            return texture.SampleGrad(gLinearWrapMirrorSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_WRAP_CLAMP:
            return texture.SampleGrad(gLinearWrapClampSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_MIRROR_WRAP:
            return texture.SampleGrad(gLinearMirrorWrapSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_MIRROR_MIRROR:
            return texture.SampleGrad(gLinearMirrorMirrorSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_MIRROR_CLAMP:
            return texture.SampleGrad(gLinearMirrorClampSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_CLAMP_WRAP:
            return texture.SampleGrad(gLinearClampWrapSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_CLAMP_MIRROR:
            return texture.SampleGrad(gLinearClampMirrorSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        case NATIVE_SAMPLER_CLAMP_CLAMP:
        default:
            return texture.SampleGrad(gLinearClampClampSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
    }
}
#endif

float4 sampleTileTextureNative(const GPUTile gpuTile, uint nativeSampler, int2 texelBaseInt) {
#ifdef WEBGPU
    return loadTileTexture(gpuTile, int3(nativeSamplerTexel(nativeSampler, texelBaseInt, gpuTile.textureDimensions.xy), 0));
#else
#define TEXTURE_CALL(texture) sampleTextureNative(texture, nativeSampler, texelBaseInt, gpuTile.textureDimensions.xy)
    TILE_TEXTURE_SWITCH(gpuTile, TEXTURE_CALL)
#undef TEXTURE_CALL
#endif
}

float4 sampleTileTextureGrad(const GPUTile gpuTile, uint nativeSampler, float2 nativeUVCoord, float2 ddxUVNorm, float2 ddyUVNorm) {
#ifdef PER_DRAW_TEXTURES
    // The renderer bound the slot's sampler for nativeSampler (FramebufferRenderer::updateDrawTextureSets).
#define TEXTURE_SAMPLER_CALL(texture, sampler) texture.SampleGrad(sampler, nativeUVCoord, ddxUVNorm, ddyUVNorm)
    TILE_TEXTURE_SAMPLER_SWITCH(gpuTile, TEXTURE_SAMPLER_CALL)
#undef TEXTURE_SAMPLER_CALL
#else
#define TEXTURE_CALL(texture) sampleTextureGrad(texture, nativeSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm)
    TILE_TEXTURE_SWITCH(gpuTile, TEXTURE_CALL)
#undef TEXTURE_CALL
#endif
}

float4 sampleTextureLevel(const RDPTile rdpTile, const GPUTile gpuTile, bool filterBilerp, bool filterAverage, bool linearFiltering, float2 uvCoord, uint tlut, bool canDecodeTMEM, uint mipLevel, bool usesHDR) {
    float2 tcScale = gpuTile.tcScale;
    float mipScale = float(1U << mipLevel);
    uvCoord /= mipScale;
    tcScale /= mipScale;
    
    int2 texelBaseInt = floor(uvCoord);
    bool filtering = or(filterBilerp, linearFiltering);
    float4 samples[4];
    const uint nativeSampler = rdpTile.nativeSampler;
    bool gpuTileUsesTMEM = canDecodeTMEM && gpuTileFlagRawTMEM(gpuTile.flags);
#if defined(WEBGPU) && defined(PER_DRAW_TEXTURES) && !USE_FOR_LOOPS
    // Every tap's texel is found first, by the native sampler's address mode or by the tile's clamp, wrap and mirror,
    // and the taps are then loaded in one switch over the draw's texture slots (loadTileTexels()). A switch for each
    // tap and for each way of finding its texel left a pixel shader whose slot is picked per pixel (mip LOD) with 32
    // switches of 8 loads: one of GoldenEye's, 224 KB of WGSL, is 185 KB this way with half the loads, and builds in
    // about 60% of the time in Firefox 158 and Chrome 155 (RTX 2070). GoldenEye's prebuilt list is 16.3 MB of WGSL
    // instead of 19.0, and Firefox, which builds a section one shader at a time while the game waits, waited 8.7-9.4 s
    // for its boot and 14.0-21.7 s for Dam against 10.4-11.6 and 17.7-23.3 (three runs each, interleaved). The
    // ubershader reads its taps in a loop (USE_FOR_LOOPS), a load site already, and keeps it.
    const bool nativeAddress = (nativeSampler != NATIVE_SAMPLER_NONE) && !gpuTileUsesTMEM;
    int2 texels[4];
    [unroll]
    for (int i = 0; i < 4; i++) {
        const int2 texelInt = texelBaseInt + int2(i >> 1, i & 1);
        if (nativeAddress) {
            texels[i] = nativeSamplerTexel(nativeSampler, texelInt, gpuTile.textureDimensions.xy);
        }
        else {
            texels[i] = clampWrapMirrorTexel(rdpTile, gpuTile, tcScale, texelInt);
        }
    }

    if (gpuTileUsesTMEM) {
        samples[0] = sampleTMEM(texels[0], rdpTile.siz, rdpTile.fmt, rdpTile.address, rdpTile.stride, tlut, rdpTile.palette, tileTMEM(gpuTile));
        if (filtering) {
            samples[1] = sampleTMEM(texels[1], rdpTile.siz, rdpTile.fmt, rdpTile.address, rdpTile.stride, tlut, rdpTile.palette, tileTMEM(gpuTile));
            samples[2] = sampleTMEM(texels[2], rdpTile.siz, rdpTile.fmt, rdpTile.address, rdpTile.stride, tlut, rdpTile.palette, tileTMEM(gpuTile));
            samples[3] = sampleTMEM(texels[3], rdpTile.siz, rdpTile.fmt, rdpTile.address, rdpTile.stride, tlut, rdpTile.palette, tileTMEM(gpuTile));
        }
    }
    else {
        loadTileTexels(gpuTile, texels, nativeAddress ? 0 : mipLevel, filtering, samples);
    }
#else
    if ((nativeSampler == NATIVE_SAMPLER_NONE) || gpuTileUsesTMEM) {
#if USE_FOR_LOOPS
        int numSamples = select_uint(filtering, 4, 1);
        [unroll]
        for (int i = 0; i < numSamples; i++) {
            samples[i] = clampWrapMirrorSample(rdpTile, gpuTile, tcScale, texelBaseInt + int2(i >> 1, i & 1), tlut, gpuTileUsesTMEM, mipLevel);
        }
#else
        samples[0] = clampWrapMirrorSample(rdpTile, gpuTile, tcScale, texelBaseInt, tlut, gpuTileUsesTMEM, mipLevel);
        
        if (filtering) {
            samples[1] = clampWrapMirrorSample(rdpTile, gpuTile, tcScale, texelBaseInt + int2(0, 1), tlut, gpuTileUsesTMEM, mipLevel);
            samples[2] = clampWrapMirrorSample(rdpTile, gpuTile, tcScale, texelBaseInt + int2(1, 0), tlut, gpuTileUsesTMEM, mipLevel);
            samples[3] = clampWrapMirrorSample(rdpTile, gpuTile, tcScale, texelBaseInt + int2(1, 1), tlut, gpuTileUsesTMEM, mipLevel);
        }
#endif
    }
    else {
#if USE_FOR_LOOPS
        int numSamples = select_uint(filtering, 4, 1);
        [unroll]
        for (int i = 0; i < numSamples; i++) {
            samples[i] = sampleTileTextureNative(gpuTile, nativeSampler, texelBaseInt + int2(i >> 1, i & 1));
        }
#else
        samples[0] = sampleTileTextureNative(gpuTile, nativeSampler, texelBaseInt);
        
        if (filtering) {
            samples[1] = sampleTileTextureNative(gpuTile, nativeSampler, texelBaseInt + int2(0, 1));
            samples[2] = sampleTileTextureNative(gpuTile, nativeSampler, texelBaseInt + int2(1, 0));
            samples[3] = sampleTileTextureNative(gpuTile, nativeSampler, texelBaseInt + int2(1, 1));
        }
#endif
    }
#endif
    
    float4 sample00 = samples[0];
    if (filtering) {
        float2 fracPart = uvCoord - texelBaseInt;
        float4 sample01 = samples[1];
        float4 sample10 = samples[2];
        float4 sample11 = samples[3];
        if (linearFiltering) {
            return lerp(lerp(sample00, sample10, fracPart.x), lerp(sample01, sample11, fracPart.x), fracPart.y);
        }
        else {
            const bool useAverage = filterAverage && all(abs(fracPart - 0.5f) <= (1.0f / LowPrecision));
            if (filterAverage && useAverage) {
                return (sample00 + sample01 + sample10 + sample11) / 4.0f;
            }
            else {
            // Originally written by ArthurCarvalho
            // Sourced from https://www.emutalk.net/threads/emulating-nintendo-64-3-sample-bilinear-filtering-using-shaders.54215/
                float4 tri0 = lerp(sample00, sample10, fracPart.x) + (sample01 - sample00) * fracPart.y;
                float4 tri1 = lerp(sample11, sample01, 1.0f - fracPart.x) + (sample10 - sample11) * (1.0f - fracPart.y);
                return lerp(tri0, tri1, step(1.0f, fracPart.x + fracPart.y));
            }
        }
    }
    else {
        return sample00;
    }
}

float4 sampleTexture(OtherMode otherMode, RenderFlags renderFlags, float2 inputUV, float2 ddxUV, float2 ddyUV, const RDPTile rdpTile, const GPUTile gpuTile, bool nextPixelBug) {
    const bool texturePerspective = (otherMode.textPersp() == G_TP_PERSP);
    const bool applyCorrection = (!texturePerspective && !renderFlagRect(renderFlags));

    // Compensates the lack of perspective division for the UVs based on observed hardware behavior.
    const float perspCorrectionMod = applyCorrection ? 0.5f : 1.0f;
    float2 uvCoord = inputUV * perspCorrectionMod * float2(rdpTile.shifts, rdpTile.shiftt);

#if SIMULATE_LOW_PRECISION
    if (!gpuTileFlagHighRes(gpuTile.flags)) {
        // Simulates the lower precision of the hardware's coordinate interpolation.
        // This is not simulated if a texture replacement is being used.
        uvCoord = round(uvCoord * LowPrecision) / LowPrecision;
    }
#endif
    
    // Simulate the next pixel bug when it's a rect by just shifting the UV coordinates to the right by one pixel.
    // The actual bug probably requires wrapping around in texture memory to the next row when on the edge instead.
    if (nextPixelBug && renderFlagRect(renderFlags)) {
        uvCoord.x += 1.0f;
    }
    
    const bool flagShiftedByHalf = gpuTileFlagShiftedByHalf(gpuTile.flags);
    if (flagShiftedByHalf) {
        uvCoord += float2(0.5f, 0.5f);
    }
    
    uvCoord *= gpuTile.tcScale;
    uvCoord -= (float2(rdpTile.uls, rdpTile.ult) * gpuTile.ulScale) / 4.0f;
    
    const uint filter = otherMode.textFilt();
    const uint cycleType = otherMode.cycleType();
    const bool filterBilerp = (filter != G_TF_POINT) && (cycleType != G_CYC_COPY);
    const bool filterAverage = (filter == G_TF_AVERAGE);
    const bool linearFiltering = renderFlagLinearFiltering(renderFlags);
    
#if FIX_UPSCALING_PRECISION
    // Account for the fact that scaling can result in less than perfect results that affect the quality of point filtering.
    // This helps with artifacts caused by copying high resolution tiles onto other textures in non-square ratios.
    if (!filterBilerp && !linearFiltering && gpuTileFlagHighRes(gpuTile.flags)) {
        const float HighResPrecision = 8.0f;
        uvCoord = round(uvCoord * HighResPrecision) / HighResPrecision;
    }
#endif
    
    const uint tlut = otherMode.textLUT();
    const bool canDecodeTMEM = renderFlagCanDecodeTMEM(renderFlags);
    const bool usesHDR = renderFlagUsesHDR(renderFlags);
    const uint nativeSampler = rdpTile.nativeSampler;
    const bool flagHasMipmaps = gpuTileFlagHasMipmaps(gpuTile.flags);
    uint numRDPSamples = 0;
    uint RDPMipLevels[2];
    RDPMipLevels[0] = 0;
    RDPMipLevels[1] = 0;
    float mip;

    // Determine the RDP sample count and mip levels.
    if (flagHasMipmaps) {
        if (nativeSampler == NATIVE_SAMPLER_NONE) {
            float2 ddxUVScaled = ddxUV * gpuTile.tcScale;
            float2 ddyUVScaled = ddyUV * gpuTile.tcScale;
            float ddMax = max(dot(ddxUVScaled, ddxUVScaled), dot(ddyUVScaled, ddyUVScaled));
            float mipBias = -0.25f;
            mip = 0.5 * log2(ddMax) + mipBias;
            float maxMip = float(gpuTile.textureDimensions.z - 1);
            RDPMipLevels[0] = min(floor(mip), maxMip);
            RDPMipLevels[1] = min(floor(mip) + 1, maxMip);
            numRDPSamples = 2;
        }
        else {
            // Must normalize the texture coordinate and the derivatives.
            float2 nativeUVCoord = uvCoord / gpuTile.textureDimensions.xy;
            float2 originalSize = gpuTile.textureDimensions.xy / gpuTile.tcScale;
            float2 ddxUVNorm = ddxUV / originalSize;
            float2 ddyUVNorm = ddyUV / originalSize;
            
            return sampleTileTextureGrad(gpuTile, nativeSampler, nativeUVCoord, ddxUVNorm, ddyUVNorm);
        }
    }
    else {
        numRDPSamples = 1;
    }
    
    float4 textureSamples[2];

    // Perform the RDP sampling.
#if USE_FOR_LOOPS
    [unroll]
    for (uint i = 0; i < numRDPSamples; i++) {
        textureSamples[i] = sampleTextureLevel(rdpTile, gpuTile, filterBilerp, filterAverage, linearFiltering, uvCoord, tlut, canDecodeTMEM, RDPMipLevels[i], usesHDR);
    }
#elif defined(WEBGPU) && defined(PER_DRAW_TEXTURES)
    // A level is its four taps' texels found and loaded (sampleTextureLevel()), most of a pixel shader with mip LOD, so
    // the two levels are sampled in a loop the shader keeps ([loop]) and that code is in it once, not twice. Each index
    // is a constant, or dxc keeps the arrays in memory. With RasterPS's loop over its two tiles (sampleDrawTile()), a
    // prebuilt pixel shader with mip LOD is about 70 KB of WGSL instead of 185 (RasterPS.hlsl has what Firefox saves).
    [loop]
    for (uint i = 0; i < numRDPSamples; i++) {
        const float4 levelSample = sampleTextureLevel(rdpTile, gpuTile, filterBilerp, filterAverage, linearFiltering, uvCoord, tlut, canDecodeTMEM, (i == 0) ? RDPMipLevels[0] : RDPMipLevels[1], usesHDR);
        if (i == 0) {
            textureSamples[0] = levelSample;
        }
        else {
            textureSamples[1] = levelSample;
        }
    }
#else
    textureSamples[0] = sampleTextureLevel(rdpTile, gpuTile, filterBilerp, filterAverage, linearFiltering, uvCoord, tlut, canDecodeTMEM, RDPMipLevels[0], usesHDR);
    
    if (numRDPSamples > 1) {
        textureSamples[1] = sampleTextureLevel(rdpTile, gpuTile, filterBilerp, filterAverage, linearFiltering, uvCoord, tlut, canDecodeTMEM, RDPMipLevels[1], usesHDR);
    }
#endif

    // Compute the result from the RDP samples.
    float4 textureColor;
    if (!flagHasMipmaps) {
        textureColor = textureSamples[0];
    }
    else {
        float4 hiLevel = textureSamples[0];
        float4 loLevel = textureSamples[1];
        textureColor = lerp(hiLevel, loLevel, frac(mip));
    }
    
    // Alpha channel in framebuffer textures represent the coverage. A modulo operation must be performed
    // to get the value that would correspond to the alpha channel when it's sampled.
    if (gpuTileFlagAlphaIsCvg(gpuTile.flags)) {
        const float cvgRange = usesHDR ? 65535.0f : 255.0f;
        int cvgModulo = round(textureColor.a * cvgRange) % 8;
        textureColor.a = (cvgModulo & 0x4) ? 1.0f : 0.0f;
    }
    
    return textureColor;
}