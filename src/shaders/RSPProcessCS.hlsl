//
// RT64
//

#include "shared/rt64_rsp_fog.h"
#include "shared/rt64_rsp_light.h"
#include "shared/rt64_rsp_lookat.h"
#include "shared/rt64_rsp_viewport.h"

#include "TextureGen.hlsli"
#include "WebGPU.hlsli"

#define GROUP_SIZE 64

struct RSPProcessCB {
    uint vertexStart;
    uint vertexCount;
    float prevFrameWeight;
    float curFrameWeight;
#ifdef PACKED_RSP_INPUTS
    // Where each input starts in srcInputs, in bytes, in the order RSPProcessor::process() lists them.
    uint posOffset;
    uint velOffset;
    uint tcOffset;
    uint tcVelOffset;
    uint normColOffset;
    uint viewProjIndicesOffset;
    uint worldIndicesOffset;
    uint fogIndicesOffset;
    uint lightIndicesOffset;
    uint lightCountsOffset;
    uint lookAtIndicesOffset;
    uint viewportsOffset;
    uint fogsOffset;
    uint lightsOffset;
    uint lookAtsOffset;
    uint viewProjTransformsOffset;
    uint worldTransformsOffset;
#endif
};

[[vk::push_constant]] ConstantBuffer<RSPProcessCB> gConstants : register(b0);

#ifdef PACKED_RSP_INPUTS
// Every input in one buffer (WebGPU.hlsli says why), read where the push constants say each one starts.
ByteAddressBuffer srcInputs : register(t1);
float loadPos(uint i) { return loadFloatAt(srcInputs, gConstants.posOffset + i * 4); }
float loadVel(uint i) { return loadFloatAt(srcInputs, gConstants.velOffset + i * 4); }
float loadTc(uint i) { return loadFloatAt(srcInputs, gConstants.tcOffset + i * 4); }
float loadTcVel(uint i) { return loadFloatAt(srcInputs, gConstants.tcVelOffset + i * 4); }
uint loadCol(uint i) { return loadUint8At(srcInputs, gConstants.normColOffset + i); }
int loadNorm(uint i) { return loadInt8At(srcInputs, gConstants.normColOffset + i); }
uint loadViewProjIndex(uint i) { return loadUint16At(srcInputs, gConstants.viewProjIndicesOffset + i * 2); }
uint loadWorldIndex(uint i) { return loadUint16At(srcInputs, gConstants.worldIndicesOffset + i * 2); }
uint loadFogIndex(uint i) { return loadUint16At(srcInputs, gConstants.fogIndicesOffset + i * 2); }
uint loadLightIndex(uint i) { return loadUint16At(srcInputs, gConstants.lightIndicesOffset + i * 2); }
uint loadLightCount(uint i) { return loadUint8At(srcInputs, gConstants.lightCountsOffset + i); }
uint loadLookAtIndex(uint i) { return loadUint16At(srcInputs, gConstants.lookAtIndicesOffset + i * 2); }
float4x4 loadViewProjTransform(uint i) { return loadFloat4x4At(srcInputs, gConstants.viewProjTransformsOffset + i * 64); }
float4x4 loadWorldTransform(uint i) { return loadFloat4x4At(srcInputs, gConstants.worldTransformsOffset + i * 64); }

RSPViewport loadViewport(uint i) {
    const uint address = gConstants.viewportsOffset + i * 32;
    RSPViewport viewport;
    viewport.scale = loadFloat4At(srcInputs, address).xyz;
    viewport.translate = loadFloat4At(srcInputs, address + 16).xyz;
    return viewport;
}

RSPFog loadFog(uint i) {
    const uint address = gConstants.fogsOffset + i * 8;
    RSPFog fog;
    fog.mul = loadFloatAt(srcInputs, address);
    fog.offset = loadFloatAt(srcInputs, address + 4);
    return fog;
}

RSPLight loadLight(uint i) {
    const uint address = gConstants.lightsOffset + i * 48;
    RSPLight light;
    light.posDir = loadFloat4At(srcInputs, address).xyz;
    light.kc = loadUint32At(srcInputs, address + 12);
    light.col = loadFloat4At(srcInputs, address + 16).xyz;
    light.kl = loadUint32At(srcInputs, address + 28);
    light.colc = loadFloat4At(srcInputs, address + 32).xyz;
    light.kq = loadUint32At(srcInputs, address + 44);
    return light;
}

RSPLookAt loadLookAt(uint i) {
    const uint address = gConstants.lookAtsOffset + i * 32;
    RSPLookAt lookAt;
    lookAt.x = loadFloat4At(srcInputs, address).xyz;
    lookAt.y = loadFloat4At(srcInputs, address + 16).xyz;
    return lookAt;
}
#else
TYPED_BUFFER(float) srcPos : register(t1);
TYPED_BUFFER(float) srcVel : register(t2);
TYPED_BUFFER(float) srcTc : register(t3);
TYPED_BUFFER(float) srcTcVel : register(t4);
TYPED_BUFFER(uint) srcCol : register(t5);
TYPED_BUFFER(int) srcNorm : register(t6);
TYPED_BUFFER(uint) srcViewProjIndices : register(t7);
TYPED_BUFFER(uint) srcWorldIndices : register(t8);
TYPED_BUFFER(uint) srcFogIndices : register(t9);
TYPED_BUFFER(uint) srcLightIndices : register(t10);
TYPED_BUFFER(uint) srcLightCounts : register(t11);
TYPED_BUFFER(uint) srcLookAtIndices : register(t12);
StructuredBuffer<RSPViewport> rspViewportVector : register(t13);
StructuredBuffer<RSPFog> rspFogVector : register(t14);
StructuredBuffer<RSPLight> rspLightVector : register(t15);
StructuredBuffer<RSPLookAt> rspLookAtVector : register(t16);
StructuredBuffer<float4x4> viewProjTransforms : register(t17);
StructuredBuffer<float4x4> worldTransforms : register(t18);
float loadPos(uint i) { return loadFloat(srcPos, i); }
float loadVel(uint i) { return loadFloat(srcVel, i); }
float loadTc(uint i) { return loadFloat(srcTc, i); }
float loadTcVel(uint i) { return loadFloat(srcTcVel, i); }
uint loadCol(uint i) { return loadUint8(srcCol, i); }
int loadNorm(uint i) { return loadInt8(srcNorm, i); }
uint loadViewProjIndex(uint i) { return loadUint16(srcViewProjIndices, i); }
uint loadWorldIndex(uint i) { return loadUint16(srcWorldIndices, i); }
uint loadFogIndex(uint i) { return loadUint16(srcFogIndices, i); }
uint loadLightIndex(uint i) { return loadUint16(srcLightIndices, i); }
uint loadLightCount(uint i) { return loadUint8(srcLightCounts, i); }
uint loadLookAtIndex(uint i) { return loadUint16(srcLookAtIndices, i); }
float4x4 loadViewProjTransform(uint i) { return viewProjTransforms[i]; }
float4x4 loadWorldTransform(uint i) { return worldTransforms[i]; }
RSPViewport loadViewport(uint i) { return rspViewportVector[i]; }
RSPFog loadFog(uint i) { return rspFogVector[i]; }
RSPLight loadLight(uint i) { return rspLightVector[i]; }
RSPLookAt loadLookAt(uint i) { return rspLookAtVector[i]; }
#endif

RWStructuredBuffer<float4> dstPos : register(u19);
RWStructuredBuffer<float2> dstTc : register(u20);
RWStructuredBuffer<float4> dstCol : register(u21);

[numthreads(GROUP_SIZE, 1, 1)]
void CSMain(uint vertexIndex : SV_DispatchThreadID) {
    if (vertexIndex >= gConstants.vertexCount) {
        return;
    }

    const uint vertexOffsetIndex = gConstants.vertexStart + vertexIndex;
    const uint viewProjIndex = loadViewProjIndex(vertexOffsetIndex);
    const uint transformIndex = loadWorldIndex(vertexOffsetIndex);
    const float4x4 viewProjMat = loadViewProjTransform(viewProjIndex);
    const float4x4 worldMat = loadWorldTransform(transformIndex);
    const uint posIndex = vertexOffsetIndex * 3;
    const uint normColIndex = vertexOffsetIndex * 4;
    const float3 norm = float3(
        loadNorm(normColIndex + 0) / 127.0f,
        loadNorm(normColIndex + 1) / 127.0f,
        loadNorm(normColIndex + 2) / 127.0f
    );

    // NDC Position.
    const float3 pos = float3(loadPos(posIndex + 0), loadPos(posIndex + 1), loadPos(posIndex + 2));
    const float3 vel = float3(loadVel(posIndex + 0), loadVel(posIndex + 1), loadVel(posIndex + 2));
    float4 tfPos = mul(mul(viewProjMat, worldMat), float4(pos - vel * (1.0f - gConstants.curFrameWeight), 1.0f));

    // Fog.
    const uint fogIndex = loadFogIndex(vertexOffsetIndex);
    float4 vertexColor;
    if (fogIndex > 0) {
        const RSPFog rspFog = loadFog(fogIndex - 1);
        const float fogAlpha = ((max(tfPos.z, 0) / tfPos.w) * rspFog.mul + rspFog.offset);
        vertexColor.a = clamp(fogAlpha / 255.0f, 0.0f, 1.0f);
    }
    else {
        vertexColor.a = loadCol(normColIndex + 3) / 255.0f;
    }

    // Texgen.
    const uint tcIndex = vertexOffsetIndex * 2;
    const uint lookAtIndex = loadLookAtIndex(vertexOffsetIndex);
    float2 tc = float2(loadTc(tcIndex + 0), loadTc(tcIndex + 1));
    if (lookAtIndex & RSP_LOOKAT_INDEX_ENABLED) {
        const bool textureGenLinear = lookAtIndex & RSP_LOOKAT_INDEX_LINEAR;
        const uint extractedIndex = (lookAtIndex >> RSP_LOOKAT_INDEX_SHIFT);
        const RSPLookAt lookAt = loadLookAt(extractedIndex);
        tc = computeTextureGen(tc, norm, lookAt, textureGenLinear, worldMat);
    }
    else {
        float2 tcVel = float2(loadTcVel(tcIndex + 0), loadTcVel(tcIndex + 1));
        tc -= tcVel * (1.0f - gConstants.curFrameWeight);
    }

    // Lighting.
    const uint lightIndex = loadLightIndex(vertexOffsetIndex);
    const uint lightCount = loadLightCount(vertexOffsetIndex);
    if (lightCount > 0) {
        const uint ambientIndex = lightIndex + lightCount - 1;
        float3 resultColor = loadLight(ambientIndex).col;
        for (uint i = lightIndex; i < ambientIndex; i++) {
            const RSPLight light = loadLight(i);
            if (light.kc > 0) {
                resultColor += computePosLight(pos, norm, light, worldMat);
            }
            else {
                resultColor += computeDirLight(norm, light, worldMat);
            }
        }

        vertexColor.rgb = min(resultColor, 1.0f);
    }
    else {
        vertexColor.rgb = float3(
            loadCol(normColIndex + 0) / 255.0f, 
            loadCol(normColIndex + 1) / 255.0f, 
            loadCol(normColIndex + 2) / 255.0f
        );
    }
    
    // HACK: For handling geometry exactly at the near clip plane of the viewport. This hack can
    // probably be removed once the behavior of the RSP has been reviewed in this case.
    if (tfPos.w == 0.0f) {
        tfPos.w = 1e-6f;
    }

    // Convert to N64 screen position.
    const RSPViewport rspViewport = loadViewport(viewProjIndex);
    const float3 ndcPos = tfPos.xyz / float3(tfPos.w, -tfPos.w, tfPos.w);
    const float4 screenPos = float4(ndcPos * rspViewport.scale + rspViewport.translate, tfPos.w);
    dstPos[vertexOffsetIndex] = screenPos;
    dstTc[vertexOffsetIndex] = tc;
    dstCol[vertexOffsetIndex] = vertexColor;
}