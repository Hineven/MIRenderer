#ifndef TRANSFORM_HLSL
#define TRANSFORM_HLSL

float3x3 BuildRotationMatrix(float4 Quaterion) {
    return float3x3(
        1 - 2 * (Quaterion.y * Quaterion.y + Quaterion.z * Quaterion.z), 2 * (Quaterion.x * Quaterion.y - Quaterion.w * Quaterion.z), 2 * (Quaterion.x * Quaterion.z + Quaterion.w * Quaterion.y),
        2 * (Quaterion.x * Quaterion.y + Quaterion.w * Quaterion.z), 1 - 2 * (Quaterion.x * Quaterion.x + Quaterion.z * Quaterion.z), 2 * (Quaterion.y * Quaterion.z - Quaterion.w * Quaterion.x),
        2 * (Quaterion.x * Quaterion.z - Quaterion.w * Quaterion.y), 2 * (Quaterion.y * Quaterion.z + Quaterion.w * Quaterion.x), 1 - 2 * (Quaterion.x * Quaterion.x + Quaterion.y * Quaterion.y)
    );
}

float3 TransformPoint(float3x4 Transform, float3 Point) {
    return mul(Transform, float4(Point, 1)).xyz;
}

#endif