#ifndef VOLUME_GRID_LIB_HLSL
#define VOLUME_GRID_LIB_HLSL

#include "MathConstants.hlsl"

#include "../shared/SharedVolumeGrid.hlsl"

// AABB Intersect Test
void IntersectAABB(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tNear, out float tFar) {
    const float epslion = 1e-8f;
    float3 safeInvDir;

    // Avoid being divided by zero
    safeInvDir.x = abs(rayDir.x) < epslion ? (rayDir.x >= 0.0f ? FLT_MAX : -FLT_MAX) : 1.0f / rayDir.x;
    safeInvDir.y = abs(rayDir.y) < epslion ? (rayDir.y >= 0.0f ? FLT_MAX : -FLT_MAX) : 1.0f / rayDir.y;
    safeInvDir.z = abs(rayDir.z) < epslion ? (rayDir.z >= 0.0f ? FLT_MAX : -FLT_MAX) : 1.0f / rayDir.z;

    float3 tbot = safeInvDir * (boxMin - rayOrigin);
    float3 ttop = safeInvDir * (boxMax - rayOrigin);

    float3 tmin = min(ttop, tbot);
    float3 tmax = max(ttop, tbot);

    tNear = max(max(tmin.x, tmin.y), tmin.z);
    tFar = min(min(tmax.x, tmax.y), tmax.z);
}

// DDA
float CalculateMaxDensityDDA(Texture3D<float4> tex, float3 localRayOrigin, float3 localRayDir, float tEntry, float tExit, float densityScale) {

    uint3 dim;
    tex.GetDimensions(dim.x, dim.y, dim.z);
    float3 fDim = float3(dim);

    // localOrigin + rayDir * t = position in [0, 1]
    float3 startPos = (localRayOrigin + localRayDir * tEntry) * fDim;
    float3 endPos   = (localRayOrigin + localRayDir * tExit)  * fDim;
    float3 rayDirVox = endPos - startPos;
    float lenVox = length(rayDirVox);
    if (lenVox < 0.001f) return 0.0f;
    float3 dirStep = rayDirVox / lenVox;

    // DDA Initialize
    int3 voxelPos = clamp(int3(floor(startPos)), int3(0,0,0), int3(dim) - 1);
    int3 step = sign(dirStep);
    float3 deltaT = abs(1.0f / dirStep);
    float3 distToNext;

    distToNext.x = (step.x > 0) ? (floor(startPos.x) + 1.0f - startPos.x) * deltaT.x : (startPos.x - floor(startPos.x)) * deltaT.x;
    distToNext.y = (step.y > 0) ? (floor(startPos.y) + 1.0f - startPos.y) * deltaT.y : (startPos.y - floor(startPos.y)) * deltaT.y;
    distToNext.z = (step.z > 0) ? (floor(startPos.z) + 1.0f - startPos.z) * deltaT.z : (startPos.z - floor(startPos.z)) * deltaT.z;

    float maxDensity = 0.0f;
    float tCurrent = 0.0f;
    float tEnd = lenVox;

    // DDA
    for (int i = 0; i < 1024; ++i) {
        if (tCurrent >= tEnd || any(voxelPos < 0) || any(voxelPos >= int3(dim))) {
            break;
        }

        float d = tex.Load(int4(voxelPos, 0)).a;
        maxDensity = max(maxDensity, d);

        // Choose closest axis to step forward
        if (distToNext.x < distToNext.y && distToNext.x < distToNext.z) {
            tCurrent = distToNext.x;
            distToNext.x += deltaT.x;
            voxelPos.x += step.x;
        } else if (distToNext.y < distToNext.z) {
            tCurrent = distToNext.y;
            distToNext.y += deltaT.y;
            voxelPos.y += step.y;
        } else {
            tCurrent = distToNext.z;
            distToNext.z += deltaT.z;
            voxelPos.z += step.z;
        }
    }

    return maxDensity * densityScale;
}

#endif