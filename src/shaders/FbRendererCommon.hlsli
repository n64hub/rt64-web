//
// RT64
//

#pragma once

#include "shared/rt64_fb_common.h"
#include "shared/rt64_frame_params.h"
#include "shared/rt64_framebuffer_params.h"
#include "shared/rt64_gpu_tile.h"
#include "shared/rt64_rdp_params.h"
#include "shared/rt64_rdp_tile.h"
#include "shared/rt64_render_indices.h"
#include "shared/rt64_render_params.h"

// Set 0 - Global resources and Enhancements (Raytracing or other).
ConstantBuffer<FrameParams> FrParams : register(b1, space0);
StructuredBuffer<RDPParams> instanceRDPParams : register(t2, space0);
StructuredBuffer<RDPTile> RDPTiles : register(t3, space0);
StructuredBuffer<GPUTile> GPUTiles : register(t4, space0);
StructuredBuffer<RenderIndices> instanceRenderIndices : register(t5, space0);
StructuredBuffer<RenderParams> DynamicRenderParams : register(t6, space0);
// With per-draw textures each tile has its linear sampler in set 1 instead (gTileSampler0 to 7).
#ifndef PER_DRAW_TEXTURES
SamplerState gLinearWrapWrapSampler : register(s7, space0);
SamplerState gLinearWrapMirrorSampler : register(s8, space0);
SamplerState gLinearWrapClampSampler : register(s9, space0);
SamplerState gLinearMirrorWrapSampler : register(s10, space0);
SamplerState gLinearMirrorMirrorSampler : register(s11, space0);
SamplerState gLinearMirrorClampSampler : register(s12, space0);
SamplerState gLinearClampWrapSampler : register(s13, space0);
SamplerState gLinearClampMirrorSampler : register(s14, space0);
SamplerState gLinearClampClampSampler : register(s15, space0);
#endif
// WebGPU loads texels in their place (sampleTextureNative): it guarantees only 16 samplers in a stage.
#ifndef WEBGPU
SamplerState gNearestWrapWrapSampler : register(s16, space0);
SamplerState gNearestWrapMirrorSampler : register(s17, space0);
SamplerState gNearestWrapClampSampler : register(s18, space0);
SamplerState gNearestMirrorWrapSampler : register(s19, space0);
SamplerState gNearestMirrorMirrorSampler : register(s20, space0);
SamplerState gNearestMirrorClampSampler : register(s21, space0);
SamplerState gNearestClampWrapSampler : register(s22, space0);
SamplerState gNearestClampMirrorSampler : register(s23, space0);
SamplerState gNearestClampClampSampler : register(s24, space0);
#endif

#ifdef PER_DRAW_TEXTURES
// Without descriptor indexing (WebGPU, or the RT64_PER_DRAW_TEXTURES build): see TextureSampler.hlsli.
// Set 1 - The draw's textures, one per tile of the draw.
Texture2D<float4> gTileTexture0 : register(t0, space1);
Texture2D<float4> gTileTexture1 : register(t1, space1);
Texture2D<float4> gTileTexture2 : register(t2, space1);
Texture2D<float4> gTileTexture3 : register(t3, space1);
Texture2D<float4> gTileTexture4 : register(t4, space1);
Texture2D<float4> gTileTexture5 : register(t5, space1);
Texture2D<float4> gTileTexture6 : register(t6, space1);
Texture2D<float4> gTileTexture7 : register(t7, space1);

// The linear sampler of each tile's texture, for the address modes the shader samples it with (its nativeSampler), in
// place of set 0's nine: picking one of those inside the switch over the slots made every linear sample 72 cases.
SamplerState gTileSampler0 : register(s8, space1);
SamplerState gTileSampler1 : register(s9, space1);
SamplerState gTileSampler2 : register(s10, space1);
SamplerState gTileSampler3 : register(s11, space1);
SamplerState gTileSampler4 : register(s12, space1);
SamplerState gTileSampler5 : register(s13, space1);
SamplerState gTileSampler6 : register(s14, space1);
SamplerState gTileSampler7 : register(s15, space1);

// Set 2 - The TMEM of every texture cache entry, RDP_TMEM_BYTES at RDP_TMEM_BYTES * its index.
ByteAddressBuffer gTMEMPool : register(t0, space2);

// What TextureDecoder.hlsli reads TMEM from: the entry's offset in the pool.
#define TMEM_SOURCE uint
#define TMEM_LOAD(TMEM, address) ((gTMEMPool.Load((TMEM) + ((address) & ~3U)) >> (((address) & 3U) * 8U)) & 0xFFU)
#else
// Set 1 - RGBA32 texture cache.
Texture2D<float4> gTextures[8192] : register(t0, space1);

// Set 2 - TMEM texture cache.
Texture1D<uint> gTMEM[8192] : register(t0, space2);
#endif

// Set 3 - Framebuffer.
ConstantBuffer<FramebufferParams> FbParams : register(b0, space3);
