//
// RT64
//

#include "WebGPU.hlsli"

#define GROUP_SIZE 64

struct RSPModifyCB {
    uint modifyCount;
};

[[vk::push_constant]] ConstantBuffer<RSPModifyCB> gConstants : register(b0);
TYPED_BUFFER(uint) srcModifyPos : register(t1);
RWStructuredBuffer<float4> screenPos : register(u2);

[numthreads(GROUP_SIZE, 1, 1)]
void CSMain(uint modifyIndex : SV_DispatchThreadID) {
    if (modifyIndex >= gConstants.modifyCount) {
        return;
    }

    const uint modifyOffset = modifyIndex * 2;
    const bool modifyZ = loadUint32(srcModifyPos, modifyOffset) & 0x1;
    const uint vertexIndex = (loadUint32(srcModifyPos, modifyOffset) & 0x7FFFFFFF) >> 1;
    const bool modifyLineCorner = (loadUint32(srcModifyPos, modifyOffset) >> 31) != 0;
    const uint modifyValue = loadUint32(srcModifyPos, modifyOffset + 1);
    if (modifyLineCorner) {
        // A corner of a line's quad (RT64_DRAW_LINES, RSP::drawLine), the first word's top bit set: a copy of one end,
        // moved across the line by half its width, 1.5 pixels and half a pixel more for each step of the width it was
        // drawn with. The other end's vertex is never a corner, so no thread here writes a position another reads.
        const uint otherIndex = modifyValue >> 9;
        const float2 self = screenPos[vertexIndex].xy;
        const float2 along = screenPos[otherIndex].xy - self;
        const float alongLength = length(along);
        if (alongLength > 0.0f) {
            const float halfWidth = (1.5f + float(modifyValue & 0xFF) * 0.5f) * 0.5f;
            const float side = (((modifyValue >> 8) & 0x1) != 0) ? 1.0f : -1.0f;
            screenPos[vertexIndex].xy = self + float2(-along.y, along.x) * (side * halfWidth / alongLength);
        }
    }
    else if (modifyZ) {
        screenPos[vertexIndex].z = modifyValue / 65536.0f;
    }
    else {
        const uint extX = (modifyValue >> 16) & 0xFFFF;
        const uint extY = modifyValue & 0xFFFF;
        const int intX = int(extX) << 16 >> 16;
        const int intY = int(extY) << 16 >> 16;
        screenPos[vertexIndex].x = intX / 4.0f;
        screenPos[vertexIndex].y = intY / 4.0f;
    }
}