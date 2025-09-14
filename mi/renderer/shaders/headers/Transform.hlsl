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

float3 TransformPoint(float4x4 Transform, float3 Point) {
    float4 PointW = mul(Transform, float4(Point, 1));
    return PointW.xyz / PointW.w;
}

float3 TransformVector(float3x4 Transform, float3 Vector) {
    return mul(Transform, float4(Vector, 0)).xyz;
}

float3 TransformVector(float3x3 Transform, float3 Vector) {
    return mul(Transform, Vector);
}

void GetOrthoVectors (float3 Normal, out float3 Tangent, out float3 Bitangent) {
    if (abs(Normal.x) > abs(Normal.z)) {
        Tangent = normalize(float3(-Normal.y, Normal.x, 0));
    } else {
        Tangent = normalize(float3(0, -Normal.z, Normal.y));
    }
    Bitangent = cross(Normal, Tangent);
}



#endif