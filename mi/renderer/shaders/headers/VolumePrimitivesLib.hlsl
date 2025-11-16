#pragma once

#include "Transform.hlsl"
#include "Math.hlsl"
#include "VolumePrimitive.hlsl"

bool RayIntersect(float3 Origin, float3 Direction, VolumePrimitive Primitive, float3x4 ToObject, inout float2 intersection_t, inout float std_dist_t) {
    float3x3 RotationMatrix = BuildRotationMatrix(Primitive.Rotation);
    RotationMatrix = transpose(RotationMatrix); // Transform from local-to-object rotation matrix to object-to-local
    float3 ObjectLocalOrigin = TransformPoint(ToObject, Origin);
    float3 ObjectLocalDirection = TransformVector(ToObject, Direction);
    float3 LocalOrigin = mul(RotationMatrix, ObjectLocalOrigin - Primitive.Position) / Primitive.Scales;
    float3 LocalDirection = mul(RotationMatrix, ObjectLocalDirection) / Primitive.Scales;
    float a = dot(LocalDirection, LocalDirection);
    float b = 2 * dot(LocalOrigin, LocalDirection);
    float c = dot(LocalOrigin, LocalOrigin) - 1.0f;
    float D = b * b - 4 * a * c;
    if (D < 0) {
        // No intersection
        intersection_t = 0;
        return false;
    }
    float sqrt_D = sqrt(D);
    float t1 = (-b - sqrt_D) / (2 * a);
    float t2 = (-b + sqrt_D) / (2 * a);
    intersection_t = float2(t1, t2);
    float3 Ortho = cross(LocalOrigin, LocalDirection);
    float3 ProjectedAxis = normalize(cross(LocalDirection, Ortho));
    // Minimum distance of line-to-origin (normalized to unit sphere)
    std_dist_t = abs(dot(ProjectedAxis, LocalOrigin));
    return true;
}