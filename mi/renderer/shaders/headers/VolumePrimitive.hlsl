#ifndef VOLUME_PRIMITIVE_HLSL
#define VOLUME_PRIMITIVE_HLSL

#include "Packing.hlsl"
#include "../shared/SharedVolumePrimitives.hlsl"

VolumePrimitive UnpackVolumePrimitive(PackedVolumePrimitive PackedPrimitive) {
    VolumePrimitive Primitive;
    Primitive.Position = PackedPrimitive.Position;
    float3 Rotation_xyz = UnpackSnorm4x8(PackedPrimitive.PackedRotation_OpacityHi).xyz;
    float Rotation_w = sqrt(max(1.0f - dot(Rotation_xyz, Rotation_xyz), 0.0f));
    float4 Rotation = float4(Rotation_xyz, Rotation_w);
    Primitive.Rotation = Rotation;
    Primitive.Scales = PackedPrimitive.Scales;
    float3 Color = UnpackUnorm4x8(PackedPrimitive.PackedColor_OpacityLo).rgb;
    Primitive.Color = Color;
    float Opacity = f16tof32(
        ((PackedPrimitive.PackedRotation_OpacityHi >> 24) << 8) |
        (PackedPrimitive.PackedColor_OpacityLo >> 24)
    );
    Primitive.Opacity = Opacity;
    return Primitive;
}

#endif // VOLUME_PRIMITIVE_HLSL