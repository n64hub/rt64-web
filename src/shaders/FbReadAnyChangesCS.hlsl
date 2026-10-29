//
// RT64
//

#include "Depth.hlsli"
#include "FbCommon.hlsli"
#include "WebGPU.hlsli"

[[vk::push_constant]] ConstantBuffer<FbCommonCB> gConstants : register(b0, space0);
TYPED_BUFFER(uint) gNewInput : register(t1, space0);
TYPED_BUFFER(uint) gCurInput : register(t2, space0);
RWStructuredBuffer<uint> gOutputCount : register(u3, space0);
IMAGE_FORMAT("rgba8") RWTexture2D<float4> gOutputChangeColor : register(u0, space1);
IMAGE_FORMAT("r32f") RWTexture2D<float> gOutputChangeDepth : register(u1, space1);
IMAGE_FORMAT("r32ui") RWTexture2D<uint> gOutputChangeBoolean : register(u2, space1);

[numthreads(FB_COMMON_WORKGROUP_SIZE, FB_COMMON_WORKGROUP_SIZE, 1)]
void CSMain(uint2 coord : SV_DispatchThreadID) {
    if ((coord.x < gConstants.resolution.x) && (coord.y < gConstants.resolution.y)) {
        const uint bufferIndex = coord.y * gConstants.resolution.x.x + coord.x;
        const uint2 pixelCoord = gConstants.offset + coord.xy;
        const uint newUint = loadUintSiz(gNewInput, bufferIndex, gConstants.siz);
        if (newUint != loadUintSiz(gCurInput, bufferIndex, gConstants.siz)) {
            const uint swappedUint = EndianSwapUINT(newUint, gConstants.siz);
            if (gConstants.fmt == G_IM_FMT_DEPTH) {
                const float newDepth = Depth16ToFloat(swappedUint);
                gOutputChangeDepth[pixelCoord] = newDepth;
            }
            else {
                const float4 newColor = UINTToFloat4(swappedUint, gConstants.siz, gConstants.fmt);
                gOutputChangeColor[pixelCoord] = newColor;
            }

            gOutputChangeBoolean[pixelCoord] = 1;
            InterlockedAdd(gOutputCount[0], 1);
        }
        else {
            gOutputChangeBoolean[pixelCoord] = 0;
        }
    }
}