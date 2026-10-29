//
// RT64
//

#pragma once

#include "shared/rt64_extra_params.h"
#include "shared/rt64_interleaved_raster.h"
#include "shared/rt64_point_light.h"
#include "shared/rt64_raytracing_params.h"
#include "shared/rt64_rsp_fog.h"
#include "shared/rt64_rsp_light.h"

ConstantBuffer<RaytracingParams> RtParams : register(b25, space0);
RaytracingAccelerationStructure SceneBVH : register(t26, space0);
ByteAddressBuffer posBuffer : register(t27, space0);
ByteAddressBuffer normBuffer : register(t28, space0);
ByteAddressBuffer velBuffer : register(t29, space0);
ByteAddressBuffer genTexCoordBuffer : register(t30, space0);
ByteAddressBuffer shadedColBuffer : register(t31, space0);
ByteAddressBuffer srcFogIndices : register(t32, space0);
ByteAddressBuffer srcLightIndices : register(t33, space0);
ByteAddressBuffer srcLightCounts : register(t34, space0);
ByteAddressBuffer indexBuffer : register(t35, space0);
StructuredBuffer<RSPFog> RSPFogVector : register(t36, space0);
StructuredBuffer<RSPLight> RSPLightVector : register(t37, space0);
StructuredBuffer<ExtraParams> instanceExtraParams : register(t38, space0);
StructuredBuffer<PointLight> SceneLights : register(t39, space0);
StructuredBuffer<InterleavedRaster> interleavedRasters : register(t40, space0);
RWBuffer<float4> gHitVelocityDistance : register(u41, space0);
RWBuffer<float4> gHitColor : register(u42, space0);
RWBuffer<float4> gHitNormalFog : register(u43, space0);
RWBuffer<uint> gHitInstanceId : register(u44, space0);
// WebGPU builds no ray tracing shader, so nothing writes these there, and the debugger's views (DebugPS) only read
// them: as sampled textures they need no storage format.
#ifdef WEBGPU
#   define RT_TEXTURE2D(T, NAME, REG) Texture2D<T> NAME : register(t##REG, space0)
#else
#   define RT_TEXTURE2D(T, NAME, REG) RWTexture2D<T> NAME : register(u##REG, space0)
#endif
RT_TEXTURE2D(float4, gViewDirection, 45);
RT_TEXTURE2D(float4, gShadingPosition, 46);
RT_TEXTURE2D(float4, gShadingNormal, 47);
RT_TEXTURE2D(float4, gShadingSpecular, 48);
RT_TEXTURE2D(float4, gDiffuse, 49);
RT_TEXTURE2D(int, gInstanceId, 50);
RT_TEXTURE2D(float4, gDirectLightAccum, 51);
RT_TEXTURE2D(float4, gIndirectLightAccum, 52);
RT_TEXTURE2D(float4, gReflection, 53);
RT_TEXTURE2D(float4, gRefraction, 54);
RT_TEXTURE2D(float4, gTransparent, 55);
RT_TEXTURE2D(float2, gFlow, 56);
RT_TEXTURE2D(float, gReactiveMask, 57);
RT_TEXTURE2D(float, gLockMask, 58);
RT_TEXTURE2D(float4, gNormalRoughness, 59);
RT_TEXTURE2D(float, gDepth, 60);
RT_TEXTURE2D(float4, gPrevNormalRoughness, 61);
RT_TEXTURE2D(float, gPrevDepth, 62);
RT_TEXTURE2D(float4, gPrevDirectLightAccum, 63);
RT_TEXTURE2D(float4, gPrevIndirectLightAccum, 64);
RT_TEXTURE2D(float4, gFilteredDirectLight, 65);
RT_TEXTURE2D(float4, gFilteredIndirectLight, 66);
Texture2D<float4> gBlueNoise : register(t67, space0);

// Set 3 - Framebuffer.
Texture2D<float4> gBackgroundColor : register(t1, space3);