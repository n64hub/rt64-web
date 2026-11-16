//
// RT64
//

#pragma once

// What a shader needs to also compile to WGSL for WebGPU (the build defines WEBGPU there). Elsewhere these are the
// declarations the shaders always had.
//
// WebGPU has no texel buffers, so a typed Buffer<T> or RWBuffer<T> becomes a ByteAddressBuffer and is read through
// the load function for the element width of the formatted view RT64 creates for it (R8, R16 or R32). Writing an 8 or
// 16-bit element clears and sets its bits in the 32-bit word atomically, since the neighbouring elements in the same
// word belong to other threads.
//
// A storage texture in WGSL has a format, and naga guesses rgba32float for one SPIR-V leaves formatless, so
// IMAGE_FORMAT gives each RWTexture2D the format of the texture RT64 binds to it on WebGPU.
//
// WebGPU also guarantees only 8 storage buffers per shader stage, so the RSP compute shaders built with
// PACKED_RSP_INPUTS read all their inputs from one ByteAddressBuffer, each array at a byte offset of its own (see
// RSPPackedInputs in rt64_rsp_processor.h). The *At loads read such a buffer at a byte address, on every backend.

uint loadUint32At(ByteAddressBuffer buffer, uint address) {
    return buffer.Load(address);
}

float loadFloatAt(ByteAddressBuffer buffer, uint address) {
    return asfloat(buffer.Load(address));
}

float4 loadFloat4At(ByteAddressBuffer buffer, uint address) {
    return asfloat(buffer.Load4(address));
}

uint loadUint16At(ByteAddressBuffer buffer, uint address) {
    return (buffer.Load(address & ~3U) >> ((address & 2U) << 3)) & 0xFFFFU;
}

uint loadUint8At(ByteAddressBuffer buffer, uint address) {
    return (buffer.Load(address & ~3U) >> ((address & 3U) << 3)) & 0xFFU;
}

int loadInt8At(ByteAddressBuffer buffer, uint address) {
    return int(loadUint8At(buffer, address) << 24) >> 24;
}

// A float4x4 laid out as a StructuredBuffer<float4x4> holds it: column by column, HLSL's default packing.
float4x4 loadFloat4x4At(ByteAddressBuffer buffer, uint address) {
    return transpose(float4x4(loadFloat4At(buffer, address), loadFloat4At(buffer, address + 16),
        loadFloat4At(buffer, address + 32), loadFloat4At(buffer, address + 48)));
}

#ifdef WEBGPU
#   define TYPED_BUFFER(T) ByteAddressBuffer
#   define RW_TYPED_BUFFER(T) RWByteAddressBuffer
#   define IMAGE_FORMAT(F) [[vk::image_format(F)]]

uint loadUint32(ByteAddressBuffer buffer, uint index) {
    return loadUint32At(buffer, index << 2);
}

float loadFloat(ByteAddressBuffer buffer, uint index) {
    return loadFloatAt(buffer, index << 2);
}

uint loadUint16(ByteAddressBuffer buffer, uint index) {
    return loadUint16At(buffer, index << 1);
}

uint loadUint8(ByteAddressBuffer buffer, uint index) {
    return loadUint8At(buffer, index);
}

int loadInt8(ByteAddressBuffer buffer, uint index) {
    return loadInt8At(buffer, index);
}

void storeUint16(RWByteAddressBuffer buffer, uint index, uint value) {
    const uint address = (index << 1) & ~3U;
    const uint shift = (index & 1U) << 4;
    buffer.InterlockedAnd(address, ~(0xFFFFU << shift));
    buffer.InterlockedOr(address, (value & 0xFFFFU) << shift);
}

void storeUint8(RWByteAddressBuffer buffer, uint index, uint value) {
    const uint address = index & ~3U;
    const uint shift = (index & 3U) << 3;
    buffer.InterlockedAnd(address, ~(0xFFU << shift));
    buffer.InterlockedOr(address, (value & 0xFFU) << shift);
}

// Elements of an N64 image of size siz (G_IM_SIZ_*): 4 and 8-bit images are read and written a byte at a time.
uint loadUintSiz(ByteAddressBuffer buffer, uint index, uint siz) {
    if (siz == 3) {
        return buffer.Load(index << 2);
    }
    else if (siz == 2) {
        return loadUint16(buffer, index);
    }
    else {
        return loadUint8(buffer, index);
    }
}

void storeUintSiz(RWByteAddressBuffer buffer, uint index, uint value, uint siz) {
    if (siz == 3) {
        buffer.Store(index << 2, value);
    }
    else if (siz == 2) {
        storeUint16(buffer, index, value);
    }
    else {
        storeUint8(buffer, index, value);
    }
}
#else
#   define TYPED_BUFFER(T) Buffer<T>
#   define RW_TYPED_BUFFER(T) RWBuffer<T>
#   define IMAGE_FORMAT(F)

uint loadUint32(Buffer<uint> buffer, uint index) { return buffer[index]; }
float loadFloat(Buffer<float> buffer, uint index) { return buffer[index]; }
uint loadUint16(Buffer<uint> buffer, uint index) { return buffer[index]; }
uint loadUint8(Buffer<uint> buffer, uint index) { return buffer[index]; }
int loadInt8(Buffer<int> buffer, uint index) { return buffer[index]; }
uint loadUintSiz(Buffer<uint> buffer, uint index, uint siz) { return buffer[index]; }
void storeUint16(RWBuffer<uint> buffer, uint index, uint value) { buffer[index] = value; }
void storeUintSiz(RWBuffer<uint> buffer, uint index, uint value, uint siz) { buffer[index] = value; }
#endif
