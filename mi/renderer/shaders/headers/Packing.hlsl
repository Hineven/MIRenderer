#ifndef PACKING_HLSL
#define PACKING_HLSL

#include "Math.hlsl"

float4 UnpackSnorm4x8(uint Packed)
{
    int4 PackedSignedIntegers = int4(
        // Make sure the higher bits are filled with the sign bit
        (int)(Packed << 24) >> 24,
        (int)(Packed << 16) >> 24,
        (int)(Packed << 8) >> 24,
        (int)(Packed) >> 24
    );

    float4 result = clamp((float4)PackedSignedIntegers / 127.0f, -1, 1);

    return result;
}

uint PackSnorm4x8(float4 Value)
{
    int4 PackedSignedIntegers = int4(
        clamp(Value, -1.f, 1.f) * 127.0f
    );

    return (PackedSignedIntegers.x & 0xFF) |
           ((PackedSignedIntegers.y & 0xFF) << 8) |
           ((PackedSignedIntegers.z & 0xFF) << 16) |
           ((PackedSignedIntegers.w & 0xFF) << 24);
}

float4 UnpackUnorm4x8(uint Packed)
{
    uint4 PackedUnsignedIntegers = uint4(
        (Packed & 0xFF),
        (Packed >> 8) & 0xFF,
        (Packed >> 16) & 0xFF,
        (Packed >> 24) & 0xFF
    );

    return PackedUnsignedIntegers / 255.0f;
}

uint PackUnorm4x8(float4 Value)
{
    uint4 PackedUnsignedIntegers = uint4(
        clamp(Value, 0.f, 1.f) * 255.0f
    );
    return (PackedUnsignedIntegers.x & 0xFF) |
           ((PackedUnsignedIntegers.y & 0xFF) << 8) |
           ((PackedUnsignedIntegers.z & 0xFF) << 16) |
           ((PackedUnsignedIntegers.w & 0xFF) << 24);
}

float4 UnpackQuaternion(uint Packed)
{
    float4 Unpacked = UnpackSnorm4x8(Packed);
    return normalize(Unpacked);
}

uint PackQuaternion(float4 Value)
{
    return PackSnorm4x8(Value);
}

uint PackUnorm2x16(float2 Value)
{
    uint2 PackedUnsignedIntegers = uint2(
        clamp(Value, 0.f, 1.f) * 65535.0f
    );
    return (PackedUnsignedIntegers.x & 0xFFFF) |
           ((PackedUnsignedIntegers.y & 0xFFFF) << 16);
}

float2 UnpackUnorm2x16(uint Packed)
{
    uint2 PackedUnsignedIntegers = uint2(
        Packed & 0xFFFF,
        (Packed >> 16) & 0xFFFF
    );
    return PackedUnsignedIntegers.xy / 65535.0f;
}

uint PackNormal (float3 Normal) {
    // Pack normal into 10 bits per channel
    uint Packed = 0;
    float3 U = saturateDown(Normal * 0.5f + 0.5f);
    Packed |= (uint(U.x * 1023) & 0x3FFu) << 22;
    Packed |= (uint(U.y * 1023) & 0x3FFu) << 12;
    Packed |= (uint(U.z * 1023) & 0x3FFu) << 2;
    return Packed;
}

float3 UnpackNormal (uint Packed) {
    // Unpack normal from 10 bits per channel
    float3 Normal;
    Normal.x = (float((Packed >> 22) & 0x3FFu) / 1023.0f) * 2.0f - 1.0f;
    Normal.y = (float((Packed >> 12) & 0x3FFu) / 1023.0f) * 2.0f - 1.0f;
    Normal.z = (float((Packed >> 2) & 0x3FFu) / 1023.0f) * 2.0f - 1.0f;
    return normalize(Normal);
}

uint PackUint2x16 (uint2 Value) {
    return Value.x | (Value.y << 16);
}

uint2 UnpackUint2x16 (uint Value) {
    return uint2(Value & 0xFFFF, Value >> 16);
}

#endif // PACKING_HLSL