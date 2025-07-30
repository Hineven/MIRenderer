
#include "Camera.hlsl"
#include "Transform.hlsl"
#include "Conventions.hlsl"
#include "GeometryBuffers.hlsl"
#include "../resources/CommonSamplerResources.hlsl"

Texture2D<float> HiZBuffer;

float2 LineBoxIntersect(float3 RayOrigin, float3 RayEnd, float3 BoxMin, float3 BoxMax)
{
    float3 InvRayDir = 1.0f / (RayEnd - RayOrigin);

    // find the ray intersection with each of the 3 planes defined by the minimum extrema.
    float3 FirstPlaneIntersections = (BoxMin - RayOrigin) * InvRayDir;
    // find the ray intersection with each of the 3 planes defined by the maximum extrema.
    float3 SecondPlaneIntersections = (BoxMax - RayOrigin) * InvRayDir;
    // get the closest of these intersections along the ray
    float3 ClosestPlaneIntersections = min(FirstPlaneIntersections, SecondPlaneIntersections);
    // get the furthest of these intersections along the ray
    float3 FurthestPlaneIntersections = max(FirstPlaneIntersections, SecondPlaneIntersections);

    float2 BoxIntersections;
    // find the furthest near intersection
    BoxIntersections.x = max(ClosestPlaneIntersections.x, max(ClosestPlaneIntersections.y, ClosestPlaneIntersections.z));
    // find the closest far intersection
    BoxIntersections.y = min(FurthestPlaneIntersections.x, min(FurthestPlaneIntersections.y, FurthestPlaneIntersections.z));
    // clamp the intersections to be between RayOrigin and RayEnd on the ray
    return saturate(BoxIntersections);
}

// Modified from UE5's SSRT implementation.
// Conventions are consistent with my codebase.
void ScreenSpaceRayTrace(
    CameraParameters C,
    Texture2D<float> ReversedZDepthTexture,
    Texture2D<float> NearReversedZHZBTexture,
    float3 RayWorldOrigin,
    float3 RayWorldDirection,
    float MaxTraceDistance,
    int MaxNumIterations,
    float RelTexelThickness,
    int MinWarpOccupancy,
    inout bool bHit, // If a trustworthy hit is found
    inout float3 OutHitUVZ,
    inout float3 OutLastVisibleUVZ,
    inout float OutHitTileZ
) {
    float3 RayStartUVZ;
    {
        float3 Homogeneous = TransformPoint(C.WorldToNDC, RayWorldOrigin);
        RayStartUVZ = float3(
            NDC2ToUV(Homogeneous.xy) * C.UVToHZBScale,
            Homogeneous.z
		);
    }
    float3 RayEndUVZ;
    {
        float3 ViewRayDirection = TransformVector(To3x3(C.WorldToView), RayWorldDirection);
        float OriginLinearDepth = -mul(C.WorldToView, float4(RayWorldOrigin, 1.f)).z;
        // Clamp the ray end to the near plane, avoid bad homogenous coordinates
        float RayEndWorldDistance = ViewRayDirection.z > 0.0
                                        ? min(0.99f * (OriginLinearDepth - C.NearPlane) / ViewRayDirection.z, MaxTraceDistance)
                                        : MaxTraceDistance;

        float3 RayWorldEnd = RayWorldOrigin + RayWorldDirection * RayEndWorldDistance;
        float3 Homogeneous = TransformPoint(C.WorldToNDC, RayWorldEnd);
        RayEndUVZ = float3(NDC2ToUV(Homogeneous.xy) * C.UVToHZBScale, Homogeneous.z);

        float2 ScreenEdgeIntersections = LineBoxIntersect(RayStartUVZ, RayEndUVZ, 0.xxx, float3(C.UVToHZBScale, 1));

        // Recalculate end point where it leaves the screen
        RayEndUVZ = RayStartUVZ + (RayEndUVZ - RayStartUVZ) * ScreenEdgeIntersections.y;
    }

    float3 RayDirectionUVZ = RayEndUVZ - RayStartUVZ;
    // Offset to pick which XY boundary planes to intersect
    //  First Texel
    // +------------+ << this boundary is tested agains first.
    // |   \        |
    // |    \       |
    // |     x      | << ray origin
    // |            |
    // +------------+

    float2 FloorOffset = select(RayDirectionUVZ.xy < 0, 0.0, 1.0);

    // Tracing states
    int MipLevel = -1;  // -1 stands for the full resolution depth buffer
    float CurrentT = 0; // Ray T
    float3 CurrentUVZ = RayStartUVZ;

    // Step out of current tile (HZB texel) without hit test to avoid self-intersection
    bool bStepOutOfCurrentTile = true;

    if (bStepOutOfCurrentTile)
    {
        float MipLevelForStepOut = MipLevel;
        float2 CurrentMipTexelSize = exp2(MipLevelForStepOut) * C.HZBBaseTexelSize;
        float2 CurrentMipResolution = 1.0f / CurrentMipTexelSize;

        // Go a little further from the current texel
        float2 UVOffset = .005f * CurrentMipTexelSize;
        UVOffset = select(RayDirectionUVZ.xy < 0, -UVOffset, UVOffset);

        float2 XYPlane = floor(CurrentUVZ.xy * CurrentMipResolution) + FloorOffset;
        XYPlane = XYPlane * CurrentMipTexelSize + UVOffset;

        float2 PlaneIntersections = (XYPlane - RayStartUVZ.xy) / RayDirectionUVZ.xy;
        CurrentT = min(PlaneIntersections.x, PlaneIntersections.y);
        CurrentUVZ = RayStartUVZ + CurrentT * RayDirectionUVZ;
    }

    int Iteration = 0;
    bHit = false;
    OutHitTileZ = 0;

    float LastAboveSurfaceT = CurrentT;

    // Stackless HZB traversal
    while (MipLevel >= -1
		&& Iteration < MaxNumIterations 
		&& CurrentT < 1.0f
#if SSRT_TERMINATE_ON_LOW_OCCUPANCY 
           && WaveActiveCountBits(true) > MinimumTracingThreadOccupancy
#endif
    )
    {
        float2 CurrentMipTexelSize = exp2(MipLevel) * C.HZBBaseTexelSize;
        float2 CurrentMipResolution = 1.0f / CurrentMipTexelSize;

        float2 UVOffset = .005f * CurrentMipTexelSize;
        UVOffset = select(RayDirectionUVZ.xy < 0, -UVOffset, UVOffset);

        float2 XYPlane = floor(CurrentUVZ.xy * CurrentMipResolution) + FloorOffset;
        XYPlane = XYPlane * CurrentMipTexelSize + UVOffset;

        float TileZ;

        if (MipLevel < 0) {
            // Sample from full resolution depth buffer
            float2 FullResUV = CurrentUVZ.xy * C.HZBToUVScale;
            TileZ = 1.f - ReversedZDepthTexture.SampleLevel(PointClampSampler, FullResUV, 0).r;
        } else {
            // Sample from HZB
            TileZ = 1.f - NearReversedZHZBTexture.SampleLevel(PointClampSampler, CurrentUVZ.xy, MipLevel).r;
        }

        float3 BoundaryPlanes = float3(XYPlane, TileZ);

        float3 PlaneIntersections = (BoundaryPlanes - RayStartUVZ) / RayDirectionUVZ;
        // Do not intersect with the Z-plane when the ray is heading toward the camera (may miss a closer hit)
        PlaneIntersections.z = RayDirectionUVZ.z > 0 ? PlaneIntersections.z : 1.0f;
        // Ray T used to update the current T
        float UpdateT = min(PlaneIntersections.x, PlaneIntersections.y);

        bool bAboveSurface = CurrentUVZ.z < TileZ;
        bool bSkippedTile = bAboveSurface;

        UpdateT = min(UpdateT, PlaneIntersections.z);
        bSkippedTile &= UpdateT != PlaneIntersections.z;

        if (bSkippedTile) {
            LastAboveSurfaceT = saturate(UpdateT);
        }

        CurrentT = bAboveSurface ? UpdateT : CurrentT;
        CurrentUVZ = RayStartUVZ + min(CurrentT, 1.0f) * RayDirectionUVZ;
        MipLevel += bSkippedTile ? 1 : -1;

        Iteration++;
    }

    // Somehow went below the surface
    if (MipLevel < -1 && CurrentT < 1.0f)
    {
        float2 FullResUV = CurrentUVZ.xy * C.HZBToUVScale;
        float ReversedTileZ = ReversedZDepthTexture.SampleLevel(PointClampSampler, FullResUV, 0).r;
        float TileZ = 1.f - ReversedTileZ;

        OutHitTileZ = TileZ;

        float HitLinearDepth = ZDepthToLinearDepth(C, TileZ);
        float CurLinearDepth = ZDepthToLinearDepth(C, CurrentUVZ.z);

        bHit = (CurLinearDepth - HitLinearDepth) < RelTexelThickness * max(HitLinearDepth, .00001f);

        if (!bHit)
        {
            // We went below the surface and couldn't count it as a hit, rewind to the last time we were above
            CurrentUVZ = RayStartUVZ + LastAboveSurfaceT * RayDirectionUVZ;
        }
    }

    OutHitUVZ = float3(CurrentUVZ.xy * C.HZBToUVScale, CurrentUVZ.z);
    float3 LastVisibleUVW = RayStartUVZ + LastAboveSurfaceT * RayDirectionUVZ;
    OutLastVisibleUVZ = float3(LastVisibleUVW.xy * C.HZBToUVScale, LastVisibleUVW.z);
}

