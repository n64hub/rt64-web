//
// RT64
//

#include "shared/rt64_video_interface.h"

[[vk::push_constant]] ConstantBuffer<VideoInterfaceCB> gConstants : register(b0);
Texture2D<float4> gInput : register(t1);
SamplerState gSampler : register(s2);

// Limit texture sampling to the area the VI can sample of the texture.

float4 SampleInput(float2 uv) {
#ifdef WEBGPU
    // WebGPU has no border addressing, and plume clamps it to the edge, so where the VI shows more lines or columns than
    // the target has (a framebuffer the game draws only partway down) the target's last row would repeat to the
    // picture's edge. Limiting the area to the texture makes those samples the border's black, as they are elsewhere.
    const float2 LowerRight = min(gConstants.videoResolution / gConstants.textureResolution, float2(1.0f, 1.0f));
#else
    const float2 LowerRight = gConstants.videoResolution / gConstants.textureResolution;
#endif
    const float2 HalfPixel = float2(0.5f, 0.5f) / gConstants.textureResolution;
    float2 outsideBorder = step(LowerRight, uv);
    float4 sampledColor = gInput.SampleLevel(gSampler, clamp(uv, HalfPixel, LowerRight - HalfPixel), 0);
    float4 gammaCorrectedColor = pow(sampledColor, gConstants.gamma);
    gammaCorrectedColor.rgb *= max(1.0f - outsideBorder.x - outsideBorder.y, 0.0f);
    gammaCorrectedColor.a = 1.0f;
    return gammaCorrectedColor;
}

//
// Sourced from https://www.shadertoy.com/view/csX3RH
//
float4 PixelAntialiasing(float2 uv) {
    float2 uvTexspace = uv * gConstants.videoResolution;
    float2 seam = floor(uvTexspace + 0.5f);
    uvTexspace = (uvTexspace - seam) / fwidth(uvTexspace) + seam;
    uvTexspace = clamp(uvTexspace, seam - 0.5f, seam + 0.5f);
    return SampleInput(uvTexspace / gConstants.textureResolution);
}

float4 PSMain(in float4 pos : SV_Position, in float2 uv : TEXCOORD0) : SV_TARGET {
#ifdef PIXEL_ANTIALIASING
    return PixelAntialiasing(uv);
#else
    return SampleInput((uv / gConstants.textureResolution) * gConstants.videoResolution);
#endif
}