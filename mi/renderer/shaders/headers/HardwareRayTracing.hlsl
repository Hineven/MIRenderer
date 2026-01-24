#ifndef __HARDWARE_RAY_TRACING_HLSL__
#define __HARDWARE_RAY_TRACING_HLSL__

// ============================================================================
// Hardware Ray Tracing Utilities
// ============================================================================
// This header provides common utilities for ray tracing shaders that handle:
// - Ray origin setup (from screen coordinates or world space)
// - Ray direction assignment
// - Ray TMin/TMax setup
// All Trace*Rays.hlsl shaders should share these patterns.
// ============================================================================

#include "Camera.hlsl"

// ============================================================================
// Ray Setup Function
// ============================================================================
// Sets up a RayDesc from input buffers with support for both screen-space
// and world-space ray origins.
//
// Parameters:
//   - RayIndex: Index into ray data buffers
//   - OutRay: Output RayDesc structure to be populated
//
// Expected Buffers:
//   - RayToTraceOriginScreenCoordBuffer (optional, when USE_SCREEN_COORDS is defined)
//   - RayToTraceOriginBuffer (optional, when USE_SCREEN_COORDS is NOT defined)
//   - RayToTraceDirectionBuffer (required)
//   - RWRayToTraceStateBuffer (required, contains TMin packed)
//   - RayToTraceTMaxBuffer (optional, when USE_RAY_TMAX_BUFFER is defined)
//   - G_Depth (optional, required when USE_SCREEN_COORDS is defined)
//
// Defines used:
//   - USE_SCREEN_COORDS: If defined, ray origin is recovered from screen coordinates
//   - USE_RAY_TMAX_BUFFER: If defined, use custom ray TMax from buffer, otherwise use camera far plane
//
void SetupRayDesc(uint RayIndex, out RayDesc OutRay)
{
    OutRay = (RayDesc)0;
    
    CameraParameters C = GetActiveCamera();
    
    // Set ray origin
#ifndef USE_SCREEN_COORDS
    // Ray origin in world space from buffer
    OutRay.Origin = RayToTraceOriginBuffer[RayIndex];
#else
    // Ray origin recovered from screen coordinates and depth buffer
    uint2 PixelIndex = UnpackUint2x16(RayToTraceOriginScreenCoordBuffer[RayIndex]);
    float2 UV = (PixelIndex + 0.5f) * C.InvFilmDimensions;
    float ReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, UV, 0);
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float LinearDepthOffset = max(1e-7f, LinearDepth * 2e-5f); // Offset the origin a little to avoid self-intersection
    LinearDepth = max(LinearDepth - LinearDepthOffset, LinearDepth * 0.95f);
    OutRay.Origin = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
#endif
    
    // Set ray direction
    OutRay.Direction = RayToTraceDirectionBuffer[RayIndex];
    
    // Set ray TMin from packed state buffer
    bool bHit = false;
    OutRay.TMin = UnpackRayToTraceState(RWRayToTraceStateBuffer[RayIndex], bHit);
    
    // Set ray TMax
#ifdef USE_RAY_TMAX_BUFFER
    OutRay.TMax = RayToTraceTMaxBuffer[RayIndex];
#else
    OutRay.TMax = C.FarPlane;
#endif
}

#endif // __HARDWARE_RAY_TRACING_HLSL__
