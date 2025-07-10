#ifndef VOLUME_PRIMITIVE_HLSL
#define VOLUME_PRIMITIVE_HLSL

#include "Packing.hlsl"

struct PackedVolumePrimitive {
    float3 Position;
    // Snorm4x8 packed quaterion
    uint PackedRotation;
    float3 Scales;
    // Unorm4x8 packed color and opacity
    uint PackedColorOpacity;
};

struct VolumePrimitive {
    float3 Position;
    float4 Rotation; // Quaternion
    float3 Scales;
    float3 Color;
    float Opacity;
};

VolumePrimitive UnpackVolumePrimitive(PackedVolumePrimitive Packed) {
    VolumePrimitive Result;
    Result.Position = Packed.Position;
    Result.Rotation = UnpackQuaternion(Packed.PackedRotation);
    Result.Scales = Packed.Scales;
    float4 ColorOpacity = UnpackUnorm4x8(Packed.PackedColorOpacity);
    Result.Color = ColorOpacity.xyz;
    Result.Opacity = ColorOpacity.w;
    return Result;
}

VolumePrimitive LoadVolumePrimitive(StructuredBuffer<PackedVolumePrimitive> Buffer, uint Index) {
    PackedVolumePrimitive Packed = Buffer[Index];
    return UnpackVolumePrimitive(Packed);
}

#endif // VOLUME_PRIMITIVE_HLSL