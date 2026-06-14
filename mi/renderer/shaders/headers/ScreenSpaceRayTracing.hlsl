
#include "Camera.hlsl"
#include "Transform.hlsl"
#include "Conventions.hlsl"
#include "GeometryBuffers.hlsl"
// #include "../resources/CommonSamplerResources.hlsl"

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
    // Detect invalid pixels when performing screen space ray tracing
    Texture2D<uint> FlagsTexture,
    Texture2D<uint> OrFlagsTexture,
    SamplerState PointBorder1Sampler, // A point sampler with border address mode, border color is 1
    float3 RayWorldOrigin,
    float3 RayWorldDirection,
    float MaxTraceDistance,
    int MaxNumIterations,
    float RelTexelThickness,
    int MinWarpOccupancy,
    bool bPrintDebugMessages,
    inout bool bHit, // If a trustworthy hit is found
    inout float3 OutHitUVZ,
    inout float3 OutLastVisibleUVZ,
    inout float OutHitReversedTileZ,
    inout float3 OutLastValidUVZ
) {
    float3 RayStartUVZ;
    {
        float3 Homogeneous = TransformPoint(C.WorldToNDC_ReversedZ, RayWorldOrigin);
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
        float3 Homogeneous = TransformPoint(C.WorldToNDC_ReversedZ, RayWorldEnd);
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

    // Step out of current tile (pixel / hzb pixel) without hit test to avoid self-intersection
    bool bStepOutOfCurrentTile = true;

    if (bStepOutOfCurrentTile)
    {
        float MipLevelForStepOut = MipLevel;
        float2 CurrentMipTexelSize = exp2(MipLevelForStepOut) * C.HZBBaseTexelSize;
        float2 CurrentMipResolution = 1.0f / CurrentMipTexelSize;

        float2 UVOffset = .005f * CurrentMipTexelSize;
        UVOffset = select(RayDirectionUVZ.xy < 0, -UVOffset, UVOffset);
        
        float2 XYPlane = floor(CurrentUVZ.xy * CurrentMipResolution) + FloorOffset;
        // Go a little further from the current texel border (to correctly march into the next texel)
        XYPlane = XYPlane * CurrentMipTexelSize + UVOffset;

        float2 PlaneIntersections = (XYPlane - RayStartUVZ.xy) / RayDirectionUVZ.xy;
        CurrentT = min(PlaneIntersections.x, PlaneIntersections.y);
        CurrentUVZ = RayStartUVZ + CurrentT * RayDirectionUVZ;
    }

    int Iteration = 0;
    bHit = false;
    OutHitReversedTileZ = 1;

    float LastAboveSurfaceT = CurrentT;
    float LastValidT = CurrentT;


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

        float ReversedTileZ;
        uint Flags = 0;

        if (MipLevel < 0) {
            // Sample from full resolution depth buffer
            float2 FullResUV = CurrentUVZ.xy * C.HZBToUVScale;
            ReversedTileZ = ReversedZDepthTexture.SampleLevel(PointBorder1Sampler, FullResUV, 0).r;
            int2 FullResPixel = uint2(FullResUV * C.FilmDimensions);
            if(all(FullResPixel < C.FilmDimensions) && all(FullResPixel >= 0))
                Flags = FlagsTexture.Load(uint3(FullResPixel, 0)).r;
        } else {
            // Sample from HZB
            ReversedTileZ = NearReversedZHZBTexture.SampleLevel(PointBorder1Sampler, CurrentUVZ.xy, MipLevel).r;
            int2 HiZPixel = uint2(CurrentUVZ.xy * CurrentMipResolution);
            int2 OrFlagsDimensions = C.HZBDimensions >> MipLevel;
            if(all(HiZPixel < OrFlagsDimensions) && all(HiZPixel >= 0))
                Flags = OrFlagsTexture.Load(uint3(HiZPixel, MipLevel)).r;
        }

        if(bPrintDebugMessages) {
            float2 Pixel = CurrentUVZ.xy * CurrentMipResolution;
            printf("Sampling at mip level pixel: %f, %f\n", Pixel.x, Pixel.y);
            printf("SSRT Iteration %d, MipLevel %d, CurrentT %.4f, CurrentUVZ (%.4f, %.4f, %.4f), ReversedTileZ %.4f\n", 
                Iteration, MipLevel, CurrentT, CurrentUVZ.x, CurrentUVZ.y, CurrentUVZ.z, ReversedTileZ);
        }
        
        bool bValidForSSRT = 0 == (Flags & FLAG_BITS_TEXTURE_INVALID_FOR_SSRT);

        float3 BoundaryPlanes = float3(XYPlane, ReversedTileZ);

        float3 PlaneIntersections = (BoundaryPlanes - RayStartUVZ) / RayDirectionUVZ;
        // Do not intersect with the Z-plane when the ray is heading toward the camera (may miss a closer hit)
        PlaneIntersections.z = RayDirectionUVZ.z < 0 ? PlaneIntersections.z : 1.0f;
        // Ray T used to update the current T
        float UpdateT = min(PlaneIntersections.x, PlaneIntersections.y);

        bool bAboveSurface = CurrentUVZ.z > ReversedTileZ;
        bool bSkippedTile = bAboveSurface;

        UpdateT = min(UpdateT, PlaneIntersections.z);
        bSkippedTile &= UpdateT != PlaneIntersections.z;

        if (bSkippedTile) {
            LastAboveSurfaceT = saturate(UpdateT);
            if(bValidForSSRT) {
                if(bPrintDebugMessages) {
                    printf("    UpdateT to %f\n", UpdateT);
                }
                LastValidT = saturate(UpdateT);
            }
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
        float ReversedTileZ = ReversedZDepthTexture.SampleLevel(PointBorder1Sampler, FullResUV, 0).r;

        OutHitReversedTileZ = ReversedTileZ;

        float HitZDepth  = 1 - ReversedTileZ;
        float CurrZDepth = 1 - CurrentUVZ.z;

        bHit = abs(CurrZDepth - HitZDepth) < RelTexelThickness * max(HitZDepth, .00001f);

        if (!bHit)
        {
            // We went below the surface and couldn't count it as a hit, rewind to the last time we were above
            CurrentUVZ = RayStartUVZ + LastAboveSurfaceT * RayDirectionUVZ;
        }
    }

    OutHitUVZ = float3(CurrentUVZ.xy * C.HZBToUVScale, CurrentUVZ.z);
    float3 LastVisibleUVZ = RayStartUVZ + LastAboveSurfaceT * RayDirectionUVZ;
    float3 LastValidUVZ = RayStartUVZ + LastValidT * RayDirectionUVZ;
    OutLastVisibleUVZ = float3(LastVisibleUVZ.xy * C.HZBToUVScale, LastVisibleUVZ.z);
    OutLastValidUVZ = float3(LastValidUVZ.xy * C.HZBToUVScale, LastValidUVZ.z);
}

