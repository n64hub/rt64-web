//
// RT64
//

#include "WebGPU.hlsli"

#define GROUP_SIZE 64

struct RSPWorldCB {
    uint vertexStart;
    uint vertexCount;
    float prevFrameWeight;
    float curFrameWeight;
#ifdef PACKED_RSP_INPUTS
    // Where each input starts in srcInputs, in bytes, in the order VertexProcessor::process() lists them.
    uint posOffset;
    uint velOffset;
    uint normOffset;
    uint indicesOffset;
    uint worldMatsOffset;
    uint invTWorldMatsOffset;
    uint prevWorldMatsOffset;
#endif
};

[[vk::push_constant]] ConstantBuffer<RSPWorldCB> gConstants : register(b0);

#ifdef PACKED_RSP_INPUTS
// Every input in one buffer (WebGPU.hlsli says why), read where the push constants say each one starts.
ByteAddressBuffer srcInputs : register(t1);
float loadPos(uint i) { return loadFloatAt(srcInputs, gConstants.posOffset + i * 4); }
float loadVel(uint i) { return loadFloatAt(srcInputs, gConstants.velOffset + i * 4); }
int loadNorm(uint i) { return loadInt8At(srcInputs, gConstants.normOffset + i); }
uint loadIndex(uint i) { return loadUint16At(srcInputs, gConstants.indicesOffset + i * 2); }
float4x4 loadWorldMat(uint i) { return loadFloat4x4At(srcInputs, gConstants.worldMatsOffset + i * 64); }
float4x4 loadInvTWorldMat(uint i) { return loadFloat4x4At(srcInputs, gConstants.invTWorldMatsOffset + i * 64); }
float4x4 loadPrevWorldMat(uint i) { return loadFloat4x4At(srcInputs, gConstants.prevWorldMatsOffset + i * 64); }
#else
TYPED_BUFFER(float) srcPos : register(t1);
TYPED_BUFFER(float) srcVel : register(t2);
TYPED_BUFFER(int) srcNorm : register(t3);
TYPED_BUFFER(uint) srcIndices : register(t4);
StructuredBuffer<float4x4> worldMats : register(t5);
StructuredBuffer<float4x4> invTWorldMats : register(t6);
StructuredBuffer<float4x4> prevWorldMats : register(t7);
float loadPos(uint i) { return loadFloat(srcPos, i); }
float loadVel(uint i) { return loadFloat(srcVel, i); }
int loadNorm(uint i) { return loadInt8(srcNorm, i); }
uint loadIndex(uint i) { return loadUint16(srcIndices, i); }
float4x4 loadWorldMat(uint i) { return worldMats[i]; }
float4x4 loadInvTWorldMat(uint i) { return invTWorldMats[i]; }
float4x4 loadPrevWorldMat(uint i) { return prevWorldMats[i]; }
#endif

RWStructuredBuffer<float4> dstPos : register(u8);
RWStructuredBuffer<float4> dstNorm : register(u9);
RWStructuredBuffer<float4> dstVel : register(u10);

[numthreads(GROUP_SIZE, 1, 1)]
void CSMain(uint vertexIndex : SV_DispatchThreadID) {
    if (vertexIndex >= gConstants.vertexCount) {
        return;
    }
    
    const uint vertexOffsetIndex = gConstants.vertexStart + vertexIndex;
    const uint transformIndex = loadIndex(vertexOffsetIndex);
    const uint posIndex = vertexOffsetIndex * 3;
    const uint normIndex = vertexOffsetIndex * 4;
    const float3 pos = float3(loadPos(posIndex + 0), loadPos(posIndex + 1), loadPos(posIndex + 2));
    const float3 vel = float3(loadVel(posIndex + 0), loadVel(posIndex + 1), loadVel(posIndex + 2));
    const float3 norm = float3(loadNorm(normIndex + 0), loadNorm(normIndex + 1), loadNorm(normIndex + 2));
    const float4 worldPos = mul(loadWorldMat(transformIndex), float4(pos - vel * (1.0f - gConstants.curFrameWeight), 1.0f));
    const float4 worldNorm = all(norm == 0.0f) ? float4(0.0f, 0.0f, 0.0f, 1.0f) : float4(normalize(mul(loadInvTWorldMat(transformIndex), float4(norm, 0.0f)).xyz), 1.0f);
    const float4 prevWorldPos = mul(loadPrevWorldMat(transformIndex), float4(pos - vel * (1.0f - gConstants.prevFrameWeight), 1.0f));
    dstPos[vertexOffsetIndex] = worldPos;
    dstNorm[vertexOffsetIndex] = worldNorm;
    dstVel[vertexOffsetIndex] = worldPos - prevWorldPos;
}