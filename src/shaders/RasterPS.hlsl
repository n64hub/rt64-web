//
// RT64
//

#include "shared/rt64_blender.h"
#include "shared/rt64_color_combiner.h"
#include "shared/rt64_raster_params.h"
#include "RasterRenderIndex.hlsli"

#include "Depth.hlsli"
#include "FbRendererCommon.hlsli"
#include "Library.hlsli"
#include "Random.hlsli"
#include "TextureSampler.hlsli"

[[vk::push_constant]] ConstantBuffer<RasterParams> gConstants : register(b0, space0);

#if defined(MULTISAMPLING)
Texture2DMS<float> gBackgroundDepth : register(t2, space3);

float sampleBackgroundDepth(int2 pixelPos, uint sampleCount) {
    float v = gBackgroundDepth.Load(pixelPos, 0);
    if (sampleCount >= 2) {
        v += gBackgroundDepth.Load(pixelPos, 1);
    }

    if (sampleCount >= 4) {
        v += gBackgroundDepth.Load(pixelPos, 2);
        v += gBackgroundDepth.Load(pixelPos, 3);
    }

    if (sampleCount >= 8) {
        v += gBackgroundDepth.Load(pixelPos, 4);
        v += gBackgroundDepth.Load(pixelPos, 5);
        v += gBackgroundDepth.Load(pixelPos, 6);
        v += gBackgroundDepth.Load(pixelPos, 7);
    }

    return v / float(sampleCount);
}
#elif defined(DYNAMIC_RENDER_PARAMS) || defined(SPEC_CONSTANT_RENDER_PARAMS) || defined(LIBRARY)
Texture2D<float> gBackgroundDepth : register(t2, space3);

float sampleBackgroundDepth(int2 pixelPos, uint sampleCount) {
    return gBackgroundDepth.Load(int3(pixelPos, 0));
}
#endif

#if defined(DECAL_DEPTH_BOUNDS)
// A decal's pixel shader on WebGPU, prebuilt: it reads no depth, so a decal draws in the pass that writes depth. A pass
// can read the depth it draws to only if it writes none, and RT64 elsewhere ends the pass for each decal that follows
// a draw that wrote depth and begins it again after: Banjo-Tooie ~25 times a frame, each pass ~70 us of Firefox's GPU
// process. The decal check, |pixel - surface| <= max(CoplanarDepthTolerance(surface), dz), is two depth tests against
// the surface's depth instead, in two draws with the stencil between them (RasterShader::createPipeline()): the first
// writes the highest surface depth that passes (greater-equal; RasterParams' padding0 is 1 for it) and marks the
// pixels, the second the lowest (less-equal), drawn where a mark is, which it takes back.
static float decalDepth = 0.0f;

// CoplanarDepthTolerance() is the same over each band of FloatToFixedDepth()'s exponent (0, 1, 2, 3 and more), so a
// side's bound is the farthest one of the bands gives. Two depth tests pass one interval, and the check can pass two
// pieces: within 0.0005 of where a band starts, a surface in the band below passes with its wider tolerance
// while one in the pixel's own band just past its narrower one doesn't, and the interval draws there too.
float decalDepthBound(float pixelDepth, float dz, bool high) {
    // Where the exponent becomes 1, 2 and 3: FloatToFixedDepth() rounds, so half a step below 0x20000, 0x30000, 0x38000.
    const float bandStart[4] = { 0.0f, 131071.5f / 262143.0f, 196607.5f / 262143.0f, 229375.5f / 262143.0f };
    float bound = high ? (pixelDepth + dz) : (pixelDepth - dz);
    for (uint e = 0; e < 4; e++) {
        const float tolerance = 0.0005f / float(1U << e);
        const float start = bandStart[e];
        const float end = (e < 3) ? bandStart[e + 1] : 2.0f;
        if (high && (pixelDepth + tolerance >= start)) {
            // The band's surface depths up to the pixel's plus its tolerance: the last one below the next band at most.
            bound = max(bound, min(pixelDepth + tolerance, asfloat(asuint(end) - 1U)));
        }
        else if (!high && (pixelDepth - tolerance < end)) {
            bound = min(bound, max(pixelDepth - tolerance, start));
        }
    }

    return bound;
}
#endif

#if defined(WEBGPU) && defined(PER_DRAW_TEXTURES)
// Samples texture 0 or 1 (t) as the two blocks below RasterPS's #else do. With mip LOD the tiles are picked per pixel,
// so each tile's texture slot is a switch anyway (TextureSampler.hlsli), and RasterPS samples both in a loop the shader
// keeps, which leaves sampleTexture() in it once: most of a prebuilt pixel shader with mip LOD. Without LOD a specialized
// shader knows each tile's slot and clamp modes only if it samples them apart, as two calls (one of Majora's Mask's
// with two tiles is 10 KB of WGSL that way, 60 in the loop). The ubershader always loops, and so does the spec constant
// shader the browser specializes (SPEC_CONSTANT_LOOPS). Firefox 158 builds a section's pipelines one at a time while
// the game waits, and in a test page built GoldenEye's Dam in 5.8-8.3 s against 14.8-19.1, Pilotwings 64's menus in
// 5.7-8.2 against 13.9-16.3 and Conker's Windy in 4.1-4.4 against 6.3-8.4 (RTX 2070, three runs each, interleaved, a
// busy host).
float4 sampleDrawTile(uint t, OtherMode otherMode, RenderFlags flags, int tileIndex0, int tileIndex1, float2 lowResUV, float2 highResUV, float2 ddxUV, float2 ddyUV) {
    const bool oneCycleHardwareBug = (otherMode.cycleType() == G_CYC_1CYCLE);
    const bool firstTile = (t == 0) || oneCycleHardwareBug;
    const uint tileIndex = firstTile ? tileIndex0 : tileIndex1;
    const uint globalTileIndex = instanceRenderIndices[RENDER_INDEX].rdpTileIndex + tileIndex;
    RDPTile rdpTile = RDPTiles[globalTileIndex];
    if (!renderFlagDynamicTiles(flags)) {
        rdpTile.cms = firstTile ? renderCMS0(flags) : renderCMS1(flags);
        rdpTile.cmt = firstTile ? renderCMT0(flags) : renderCMT1(flags);
        rdpTile.nativeSampler = firstTile ? renderFlagNativeSampler0(flags) : renderFlagNativeSampler1(flags);
    }

    const GPUTile gpuTile = drawGPUTile(instanceRenderIndices[RENDER_INDEX].rdpTileIndex, tileIndex);
    const float2 textureUV = gpuTileFlagHighRes(gpuTile.flags) ? highResUV : lowResUV;
    return sampleTexture(otherMode, flags, textureUV, ddxUV, ddyUV, rdpTile, gpuTile, (t == 1) && oneCycleHardwareBug);
}
#endif

LIBRARY_EXPORT bool RasterPS(const RenderParams rp, float4 vertexPosition, float2 vertexUV, float4 vertexSmoothColor, float4 vertexFlatColor,
    bool isFrontFace, out float4 resultColor, out float4 resultAlpha) 
{
    // Derivatives first, while every pixel of the quad is still running: they are undefined after a pixel returns, and
    // WGSL refuses them in control flow that can differ between pixels.
    const float ddxDepth = ddx(vertexPosition.z);
    const float ddyDepth = ddy(vertexPosition.z);
    float2 ddxVertexUV = ddx(vertexUV);
    float2 ddyVertexUV = ddy(vertexUV);

    const OtherMode otherMode = { rp.omL, rp.omH };
#if defined(DYNAMIC_RENDER_PARAMS)
    if ((otherMode.cycleType() != G_CYC_COPY) && renderFlagCulling(rp.flags) && isFrontFace) {
        return false;
    }
#endif
    
    const uint instanceIndex = instanceRenderIndices[RENDER_INDEX].instanceIndex;
    const float4 vertexColor = renderFlagSmoothShade(rp.flags) ? vertexSmoothColor : float4(vertexFlatColor.rgb, vertexSmoothColor.a);
    const ColorCombiner colorCombiner = { rp.ccL, rp.ccH };
    const bool depthClampNear = renderFlagNoN(rp.flags);
    const bool depthDecal = (otherMode.zMode() == ZMODE_DEC);
    const bool zSourcePrim = (otherMode.zSource() == G_ZS_PRIM);
    int2 pixelPosSeed = floor(vertexPosition.xy);
    uint randomSeed = initRand(FrParams.frameCount, instanceIndex * pixelPosSeed.y * pixelPosSeed.x, 16); // TODO: Review seed.

    // The F3D family of microcodes has a bug where polygons are clipped earlier than expected despite the depth being computed
    // in a wider range of values. Not accounting for this can lead to geometry showing up that should otherwise be invisible.
    // This max range was determined by using a test ROM against an LLE implementation.
    // FIXME: This should be turned off for non-F3D microcodes.
    const bool simulateDepthClipF3D = true;
    const float MaxDepth = simulateDepthClipF3D && !renderFlagRect(rp.flags) && !zSourcePrim ? (1022.0f / 1024.0f) : 1.0f;
    
    // FIXME: This can be implemented by checking feature support for the API and embedding the depth bounds into the pipeline.
    const bool pipelineDepthBounds = false;
    
    // Handle no-nearclipping by clamping the minimum depth and manually clipping above the maximum.
    if (depthClampNear) {
        // Since depth clip is disabled on the PSO so near clip can be ignored, we manually clip any values above the allowed depth.
        if (vertexPosition.z > MaxDepth) {
            return false;
        }
    }
    // Do depth clipping manually on the fragment shader if pipeline's depth bounds are not being used due to lack of hardware support.
    else if (!pipelineDepthBounds) {
        if ((vertexPosition.z < 0.0f) || (vertexPosition.z > MaxDepth)) {
            return false;
        }
    }

    if (depthDecal) {
        // Calculate the decal depth tolerance based on the depth derivatives (or the prim dz value if prim depth source is enabled).
        float dz;
        if (zSourcePrim) {
            dz = instanceRDPParams[instanceIndex].primDepth.y;
        }
        else {
            dz = (abs(ddxDepth) + abs(ddyDepth)) * FbParams.resolutionScale.y;
        }

        const float pixelDepth = select(depthClampNear, max(vertexPosition.z, 0.0f), vertexPosition.z);
#if defined(DECAL_DEPTH_BOUNDS)
        // The depth tests do the check (decalDepthBound()).
        decalDepth = decalDepthBound(pixelDepth, dz, gConstants.padding0 != 0);
#else
        // Sample the depth buffer for this pixel to compare for the decal check.
        int2 pixelPos = floor(vertexPosition.xy);
        uint sampleCount = 1U << renderFlagSampleCount(rp.flags);
        float surfaceDepth = sampleBackgroundDepth(pixelPos, sampleCount);

        // Perform the decal depth tolerance check.
        const float DepthTolerance = max(CoplanarDepthTolerance(surfaceDepth), dz);
        if (abs(pixelDepth - surfaceDepth) > DepthTolerance) {
            return false;
        }
#endif
    }
    
    float2 lowResUV = vertexUV;
    if (!renderFlagUpscale2D(rp.flags)) {
        float2 screenPos = floor(vertexPosition.xy);
        screenPos.x += FbParams.horizontalMisalignment;
        lowResUV -= fmod(screenPos.xy, FbParams.resolutionScale.yy) * float2(ddxVertexUV.x, ddyVertexUV.y);
    }

    // A high resolution tile is sampled at vertexUV, which on a rect drawn backwards (a negative dsdx or dtdy) runs
    // one native pixel behind the texel the low resolution UV picks, so its last row or column samples outside the
    // tile. With FLIPPED_RECT_HIGH_RES (RT64_FLIPPED_RECT_HIGH_RES, on with WGSL) each native pixel starts at the texel
    // it shows instead.
    float2 highResUV = vertexUV;
#if defined(FLIPPED_RECT_HIGH_RES)
    if (renderFlagRect(rp.flags) && !renderFlagUpscale2D(rp.flags)) {
        highResUV -= min(float2(ddxVertexUV.x, ddyVertexUV.y), 0.0f) * FbParams.resolutionScale.yy;
    }
#endif

    int tileIndex0 = 0;
    int tileIndex1 = 1;
    float lodFraction;
    float lodScale = 1.0f;
    if (!renderFlagUpscaleLOD(rp.flags)) {
        lodScale = FbParams.resolutionScale.y;
    }
    
    computeLOD(otherMode, instanceRenderIndices[RENDER_INDEX].rdpTileCount, instanceRDPParams[instanceIndex].primLOD, lodScale, ddxVertexUV, ddyVertexUV, tileIndex0, tileIndex1, lodFraction);

    float4 texVal0 = float4(0.0f, 0.0f, 0.0f, 1.0f);
    float4 texVal1 = float4(0.0f, 0.0f, 0.0f, 1.0f);
#if defined(WEBGPU) && defined(PER_DRAW_TEXTURES)
    // The tiles in a loop or apart: sampleDrawTile() says when.
    const bool usesTexture0 = renderFlagUsesTexture0(rp.flags);
    const bool usesTexture1 = renderFlagUsesTexture1(rp.flags);
    const bool loopTiles = usesTexture0 && usesTexture1 && (otherMode.textLOD() == G_TL_LOD);
#if !defined(DYNAMIC_RENDER_PARAMS) && !defined(SPEC_CONSTANT_LOOPS)
    if (!loopTiles) {
        if (usesTexture0) {
            texVal0 = sampleDrawTile(0, otherMode, rp.flags, tileIndex0, tileIndex1, lowResUV, highResUV, ddxVertexUV, ddyVertexUV);
        }

        if (usesTexture1) {
            texVal1 = sampleDrawTile(1, otherMode, rp.flags, tileIndex0, tileIndex1, lowResUV, highResUV, ddxVertexUV, ddyVertexUV);
        }
    }
    else
#endif
    {
        [loop]
        for (uint t = 0; t < 2; t++) {
            if (!((t == 0) ? usesTexture0 : usesTexture1)) {
                continue;
            }

            const float4 texVal = sampleDrawTile(t, otherMode, rp.flags, tileIndex0, tileIndex1, lowResUV, highResUV, ddxVertexUV, ddyVertexUV);
            if (t == 0) {
                texVal0 = texVal;
            }
            else {
                texVal1 = texVal;
            }
        }
    }
#else
    if (renderFlagUsesTexture0(rp.flags)) {
        const uint globalTileIndex = instanceRenderIndices[RENDER_INDEX].rdpTileIndex + tileIndex0;
        RDPTile rdpTile = RDPTiles[globalTileIndex];
        if (!renderFlagDynamicTiles(rp.flags)) {
            rdpTile.cms = renderCMS0(rp.flags);
            rdpTile.cmt = renderCMT0(rp.flags);
            rdpTile.nativeSampler = renderFlagNativeSampler0(rp.flags);
        }
        
        const GPUTile gpuTile = drawGPUTile(instanceRenderIndices[RENDER_INDEX].rdpTileIndex, tileIndex0);
        const float2 textureUV = gpuTileFlagHighRes(gpuTile.flags) ? highResUV : lowResUV;
        texVal0 = sampleTexture(otherMode, rp.flags, textureUV, ddxVertexUV, ddyVertexUV, rdpTile, gpuTile, false);
    }
    
    if (renderFlagUsesTexture1(rp.flags)) {
        const bool oneCycleHardwareBug = (otherMode.cycleType() == G_CYC_1CYCLE);
        const uint tileIndex = oneCycleHardwareBug ? tileIndex0 : tileIndex1;
        const uint globalTileIndex = instanceRenderIndices[RENDER_INDEX].rdpTileIndex + tileIndex;
        RDPTile rdpTile = RDPTiles[globalTileIndex];
        if (!renderFlagDynamicTiles(rp.flags)) {
            rdpTile.cms = oneCycleHardwareBug ? renderCMS0(rp.flags) : renderCMS1(rp.flags);
            rdpTile.cmt = oneCycleHardwareBug ? renderCMT0(rp.flags) : renderCMT1(rp.flags);
            rdpTile.nativeSampler = oneCycleHardwareBug ? renderFlagNativeSampler0(rp.flags) : renderFlagNativeSampler1(rp.flags);
        }
        
        const GPUTile gpuTile = drawGPUTile(instanceRenderIndices[RENDER_INDEX].rdpTileIndex, tileIndex);
        const float2 textureUV = gpuTileFlagHighRes(gpuTile.flags) ? highResUV : lowResUV;
        const uint nativeSampler = renderFlagNativeSampler1(rp.flags);
        texVal1 = sampleTexture(otherMode, rp.flags, textureUV, ddxVertexUV, ddyVertexUV, rdpTile, gpuTile, oneCycleHardwareBug);
    }
#endif

    // Color combiner.
    float4 shadeColor = vertexColor;
    float4 combinerColor;
    float alphaCompareValue;
    ColorCombiner::Inputs ccInputs;
    ccInputs.otherMode = otherMode;
    ccInputs.alphaOnly = false;
    ccInputs.texVal0 = texVal0;
    ccInputs.texVal1 = texVal1;
    ccInputs.primColor = instanceRDPParams[instanceIndex].primColor;
    ccInputs.shadeColor = shadeColor;
    ccInputs.envColor = instanceRDPParams[instanceIndex].envColor;
    ccInputs.keyCenter = instanceRDPParams[instanceIndex].keyCenter;
    ccInputs.keyScale = instanceRDPParams[instanceIndex].keyScale;
    ccInputs.lodFraction = lodFraction;
    ccInputs.primLodFrac = instanceRDPParams[instanceIndex].primLOD.x;
    ccInputs.noise = nextRand(randomSeed);
    ccInputs.K4 = (instanceRDPParams[instanceIndex].convertK[4] / 255.0f);
    ccInputs.K5 = (instanceRDPParams[instanceIndex].convertK[5] / 255.0f);
    colorCombiner.run(ccInputs, combinerColor, alphaCompareValue);
    
#if 0
    // Alpha dither.
    // TODO: To avoid increasing the alpha values here, the only viable choice would be to drop the precision down to 5-bit.
    // Since we'd rather keep the full precision of the alpha channel from the texture, this step is ignored for now.
    if (otherMode.alphaDither() != G_AD_DISABLE) {
        uint rgbDither = (otherMode.rgbDither() >> G_MDSFT_RGBDITHER) & 0x3;
        uint alphaDither = (otherMode.alphaDither() >> G_MDSFT_ALPHADITHER) & 0x3;
        float alphaDitherFloat = (AlphaDitherValue(rgbDither, alphaDither, floor(vertexPosition.xy), randomSeed) / 255.0f);
        if (!otherMode.alphaCvgSel()) {
            combinerColor.a += alphaDitherFloat;
        }
        
        shadeColor.a += alphaDitherFloat;
        alphaCompareValue += alphaDitherFloat;
    }
#endif
    
    // Alpha compare.
    if (otherMode.alphaCompare() == G_AC_DITHER) {
        if (alphaCompareValue < nextRand(randomSeed)) {
            return false;
        }
    }
    else if (otherMode.alphaCompare() == G_AC_THRESHOLD) {
        if (alphaCompareValue < instanceRDPParams[instanceIndex].blendColor.a) {
            return false;
        }
    }
    
    // Compute coverage estimation.
    const bool usesHDR = renderFlagUsesHDR(rp.flags);
    const float cvgRange = usesHDR ? 65535.0f : 255.0f;
    float resultCvg = (8.0f / cvgRange) * (otherMode.cvgXAlpha() ? combinerColor.a : 1.0f);
    
    // Discard all pixels without coverage.
    const float CoverageThreshold = 1.0f / cvgRange;
    if (resultCvg < CoverageThreshold) {
        return false;
    }
    
    // Add the blender if it can be replicated with simple emulation.
    Blender::Inputs blInputs;
    blInputs.blendColor = instanceRDPParams[instanceIndex].blendColor;
    blInputs.fogColor = instanceRDPParams[instanceIndex].fogColor;
    blInputs.shadeAlpha = shadeColor.a;
    resultColor = Blender::run(otherMode, rp.flags, blInputs, combinerColor, false);
    resultAlpha = 1.0f;
    
    // When using alpha blending, we store the blending factor into the dedicated output so the main one can be used for coverage.
    const bool alphaBlend = (otherMode.cycleType() != G_CYC_COPY) && Blender::usesAlphaBlend(otherMode);
    if (alphaBlend) {
        resultAlpha.a = resultColor.a;
    }
    
    // Preserve the value in the destination.
    if (otherMode.cvgDst() == CVG_DST_SAVE) {
        resultColor.a = 0.0f;
    }
    // Write a full coverage value regardless of the computed coverage.
    else if (otherMode.cvgDst() == CVG_DST_FULL) {
        resultColor.a = 7.0f / cvgRange;
    }
    // Write the coverage value clamped to the full value allowed.
    else if (otherMode.cvgDst() == CVG_DST_CLAMP) {
        resultColor.a = min(resultCvg, 7.0f / cvgRange);
    }
    // Write out the computed coverage. It'll be added on wrap mode.
    else {
        resultColor.a = resultCvg;
    }
    
#if defined(WEBGPU) && !defined(DUAL_SOURCE_BLENDING)
    // Without dual-source blending, which WebGPU leaves optional, an alpha-blended draw blends by the alpha of its one
    // output, so the blend factor takes the coverage's place there.
    if (alphaBlend) {
        resultColor.a = resultAlpha.a;
    }
#endif

    // Add highlight color to the last step.
    uint highlightColorUint = instanceRenderIndices[RENDER_INDEX].highlightColor;
    if (highlightColorUint > 0) {
        float4 highlightColor = RGBA32ToFloat4(highlightColorUint);
        resultColor = lerp(resultColor, highlightColor, highlightColor.a);
    }
    
#ifdef DYNAMIC_RENDER_PARAMS
    if (FrParams.viewUbershaders) {
        resultColor.rgb = lerp(resultColor.rgb, float3(1.0f, 0.0f, 0.0f), 0.5f);
    }
#endif
    
    return true;
}

#if defined(DYNAMIC_RENDER_PARAMS)
RenderParams getRenderParams() {
    uint instanceIndex = instanceRenderIndices[RENDER_INDEX].instanceIndex;
    return DynamicRenderParams[instanceIndex];
}
#elif defined(SPEC_CONSTANT_RENDER_PARAMS)
#   include "RenderParamsSpecConstants.hlsli"
#endif

#if defined(DYNAMIC_RENDER_PARAMS) || defined(SPEC_CONSTANT_RENDER_PARAMS)
void PSMain(
      in float4 vertexPosition : SV_POSITION
    , in float2 vertexUV : TEXCOORD
    , in float4 vertexSmoothColor : COLOR0
#if defined(DYNAMIC_RENDER_PARAMS) || defined(VERTEX_FLAT_COLOR)
    , nointerpolation in float4 vertexFlatColor : COLOR1
#endif
#ifdef RT64_RENDER_INDEX_BY_INSTANCE
    , nointerpolation in uint vertexRenderIndex : RENDERINDEX
#endif
#if defined(DYNAMIC_RENDER_PARAMS)
    , bool isFrontFace : SV_IsFrontFace
#endif
#if defined(WEBGPU) && !defined(DUAL_SOURCE_BLENDING)
    , [[vk::location(0)]] out float4 pixelColor : SV_TARGET0
#else
    , [[vk::location(0)]] [[vk::index(0)]] out float4 pixelColor : SV_TARGET0
    , [[vk::location(0)]] [[vk::index(1)]] out float4 pixelAlpha : SV_TARGET1
#endif
#if defined(DECAL_DEPTH_BOUNDS)
    , out float pixelDepth : SV_Depth
#endif
)
{
#ifdef RT64_RENDER_INDEX_BY_INSTANCE
    gRenderIndex = vertexRenderIndex;
#endif
#if !defined(DYNAMIC_RENDER_PARAMS)
#if !defined(VERTEX_FLAT_COLOR)
    float4 vertexFlatColor = 0.0f;
#endif
    bool isFrontFace = false;
#endif
    float4 resultColor;
    float4 resultAlpha;
    float resultDepth;
    if (!RasterPS(getRenderParams(), vertexPosition, vertexUV, vertexSmoothColor, vertexFlatColor, isFrontFace, resultColor, resultAlpha)) {
        discard;
    }

    pixelColor = resultColor;
#if defined(DECAL_DEPTH_BOUNDS)
    pixelDepth = decalDepth;
#endif
#if !defined(WEBGPU) || defined(DUAL_SOURCE_BLENDING)
    pixelAlpha = resultAlpha;
#endif
}
#endif