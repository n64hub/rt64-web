//
// RT64
//
// Sets luminance buffer to a fixed value.
//

#include "Constants.hlsli"
#include "WebGPU.hlsli"

struct HistogramSetCB {
    float luminanceValue;
};

[[vk::push_constant]] ConstantBuffer<HistogramSetCB> gConstants : register(b0);
IMAGE_FORMAT("r32f") RWTexture2D<float> LuminanceOutput : register(u1);

[numthreads(1, 1, 1)]
void CSMain(uint groupIndex : SV_GroupIndex) {
    LuminanceOutput[uint2(0, 0)] = gConstants.luminanceValue;
}