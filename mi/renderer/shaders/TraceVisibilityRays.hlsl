#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedVertex.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Random.hlsl"
#include "headers/Material.hlsl"
#include "headers/RayTracingHelpers.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "resources/RenderableResources.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"

#ifndef VISIBILITY_TRACE_TYPE
#define VISIBILITY_TRACE_TYPE 1
#endif

#define VISIBILITY_TRACE_TYPE_COARSE 0
#define VISIBILITY_TRACE_TYPE_COARSE_WITH_EXACT_VOLUME_SCATTERING 1
#define VISIBILITY_TRACE_TYPE_FULL 2

struct TraceVisibilityRaysUB {
    uint Seed;
    float VolumeScatteringEventShellHitCullingBias;
    uint2 Padding;
};
ConstantBuffer<TraceVisibilityRaysUB> UB;

RaytracingAccelerationStructure PTLAS;


Texture2D<float> G_Depth;
Texture2D<uint> G_GeometryNormal;

StructuredBuffer<uint> RayToTraceListLengthBuffer;
StructuredBuffer<uint> RayToTraceListBuffer;

StructuredBuffer<float3> RayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
RWStructuredBuffer<uint4> RWRayToTraceResultBuffer;
#else
RWStructuredBuffer<uint2> RWRayToTraceResultBuffer;
#endif

// Optional (when the starting point is exactly on a pixel center)
StructuredBuffer<uint> RayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
StructuredBuffer<float3> RayToTraceOriginBuffer;

// Optional (when the ray tmax is passed as a parameter, otherwise defaults to far plane)
StructuredBuffer<float> RayToTraceTMaxBuffer; 

#include "headers/HardwareRayTracing.hlsl"

struct [raypayload] RayPayload {
    // Hit on meshes / proxy meshes, TMax for no hits
    // For coarse visibility with exact volume scattering sampling, this is the hit distance for 
    // volume hits / surface hits.
    float HitDistance;
    float U; // Random number
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
    #error "not implemented yet"
#else
    // Packed CachedHitMaterial on the hit for coarse shading.
    uint2 PackedMaterial;
#endif
};

#include "resources/GigaVoxelResources.hlsl"
[shader("raygeneration")]
void TraceVisibilityRaysRaygen() {
#ifdef USE_RAY_LIST
    uint RayListIndex = DispatchRaysIndex().x;
    uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
    uint RayIndex = DispatchRaysIndex().x;
#endif

    RayDesc Ray;
    SetupRayDesc(RayIndex, Ray);

    RayPayload Payload = (RayPayload)0;
    Payload.HitDistance = Ray.TMax; // Default to TMax, will only be updated in a closest hit on meshes
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
    #error "not implemented yet"
#else
    Payload.PackedMaterial = MakePackedInvalidCachedHitMaterial();
#endif
    Random rng = MakeRandom(RayIndex + 0x8f71a213u, UB.Seed);
    Payload.U = rng.rand();
    TraceRay(
        PTLAS,
        // Proxy geometries of the volume primitives are built face-flipped. So culling back faces
        // means culling real front faces for volume primitives. Only real back faces from volume
        // primitives are hit when tracing the ray.
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF, // Ray mask
        0,    // SBT offset
        0,    // SBT stride (per-instance offset only)
        0,    // Miss shader index
        Ray,
        Payload
    );
    uint PackedTraceState = PackRayToTraceState(Payload.HitDistance, Payload.HitDistance < Ray.TMax);
    RWRayToTraceStateBuffer[RayIndex] = PackedTraceState;
    // Write back Radiance
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
#error "not implemented yet"
#else
    RWRayToTraceResultBuffer[RayIndex] = Payload.PackedMaterial;
#endif
}

[shader("miss")]
void TraceVisibilityRaysMiss(inout RayPayload Payload: SV_RayPayload) {
    // Leave unchanged is okay.
}

// ============================================================================
// StaticMesh anyhit: alpha test for semi-transparent surfaces
// ============================================================================
[shader("anyhit")]
void TraceVisibilityRaysAnyHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionRank   = GeometryIndex();
    uint Instance = InstanceID();  // StaticMesh: InstanceID() == RenderableIndex

    // Static mesh instance
    IntersectionMaterial Intersection = EvaluateStaticMeshRenderableIntersectionMaterial_InputTransforms(
        Instance,
        DescriptionRank,
        Triangle,
        Attributes.barycentrics,
        ObjectToWorld3x4(),
        transpose(To3x3(WorldToObject3x4()))
    );

    if(Intersection.Opacity < Payload.U) {
        Payload.U = (Payload.U - Intersection.Opacity) / (1.f - Intersection.Opacity);
        // Semi-transparent surfaces, continue tracing
        IgnoreHit();
    } else {
        // There'll be systematically more 'transparent' volumes near surfaces
        // with this tracing method. We just let that happen.
        Payload.U = Payload.U / max(Intersection.Opacity, 1e-5f);
    }
}

// ============================================================================
// ============================================================================

// ============================================================================
// ============================================================================

// ============================================================================
// StaticMesh closesthit: record hit material
// ============================================================================
[shader("closesthit")]
void TraceVisibilityRaysClosestHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
    #error "not implemented yet"
#else
    // Found a static mesh instance. Return the hit distance.
    uint Triangle          = PrimitiveIndex();
    uint DescriptionRank   = GeometryIndex();
    uint Instance = InstanceID();  // StaticMesh: InstanceID() == RenderableIndex

    Payload.HitDistance = RayTCurrent();

    IntersectionMaterial Intersection = EvaluateStaticMeshRenderableIntersectionMaterial_InputTransforms(
        Instance,
        DescriptionRank,
        Triangle,
        Attributes.barycentrics,
        ObjectToWorld3x4(),
        transpose(To3x3(WorldToObject3x4()))
    );
    float3 GeometryNormal = Intersection.GeometryNormal;
    // Flip normal if needed
    if(dot(GeometryNormal, WorldRayDirection()) > 0) {
        GeometryNormal = -GeometryNormal;
    }
    // Prefer geometry normal for better light leaks prevention
    CachedHitMaterial CachedHitMat = MakeCachedHitMaterial(Intersection.Albedo, CACHED_HIT_MATERIAL_HIT_TYPE_SURFACE, GeometryNormal);
    Payload.PackedMaterial = PackCachedHitMaterial(CachedHitMat);
#endif
}

// ============================================================================
// ============================================================================

// ============================================================================
// ============================================================================

// VolumeGrid no-op entry points for visibility rays
[shader("anyhit")]
void TraceVisibilityRaysAnyHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
}
[shader("closesthit")]
void TraceVisibilityRaysClosestHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
}

// GigaVoxel: VC chunk geometry with atlas-sampled albedo/opacity. anyHit does
// opacity free-path sampling (same as StaticMesh); closestHit caches the material.
[shader("anyhit")]
void TraceVisibilityRaysAnyHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                               BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
       // FIXME no-op
//     IntersectionMaterial Intersection = EvaluateGigaVoxelRenderableIntersectionMaterial(
//         InstanceID(), PrimitiveIndex(), Attributes.barycentrics,
//         ObjectToWorld3x4(), transpose(To3x3(WorldToObject3x4())));
//     if(Intersection.Opacity < Payload.U) {
//         Payload.U = (Payload.U - Intersection.Opacity) / (1.f - Intersection.Opacity);
//         IgnoreHit();
//     } else {
//         Payload.U = Payload.U / max(Intersection.Opacity, 1e-5f);
//     }
}
[shader("closesthit")]
void TraceVisibilityRaysClosestHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
          // FIXME no-op
//     uint Triangle = PrimitiveIndex();
//     Payload.HitDistance = RayTCurrent();
//     IntersectionMaterial Intersection = EvaluateGigaVoxelRenderableIntersectionMaterial(
//         InstanceID(), Triangle, Attributes.barycentrics,
//         ObjectToWorld3x4(), transpose(To3x3(WorldToObject3x4())));
//     float3 GeometryNormal = Intersection.GeometryNormal;
//     if(dot(GeometryNormal, WorldRayDirection()) > 0) GeometryNormal = -GeometryNormal;
//     CachedHitMaterial CachedHitMat = MakeCachedHitMaterial(Intersection.Albedo, CACHED_HIT_MATERIAL_HIT_TYPE_SURFACE, GeometryNormal);
//     Payload.PackedMaterial = PackCachedHitMaterial(CachedHitMat);
}
