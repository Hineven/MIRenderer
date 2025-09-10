#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedLight.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Light.hlsl"
#include "headers/VolumePrimitive.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "resources/BindlessTextureResources.hlsl"

// Input macros
#ifndef MAX_NUM_GRID_LIGHTS
#define MAX_NUM_GRID_LIGHTS 32
#endif
#ifndef LIGHT_GRID_NUM_CASCADES
#define LIGHT_GRID_NUM_CASCADES 6
#endif

#ifndef NUM_LIGHT_SAMPELR_SAMPLES
#define NUM_LIGHT_SAMPELR_SAMPLES 8
#endif

RWStructuredBuffer<uint> RWRayToTraceCount;
RWStructuredBuffer<uint> RWVolumeRayToTraceCount;

// All area lights
StructuredBuffer<AreaLight> LightBuffer;
RWStructuredBuffer<PackedPrecomputedLight> RWPrecomputedActiveLightBuffer;
StructuredBuffer<PackedPrecomputedLight> PrecomputedActiveLightBuffer;

RWStructuredBuffer<uint> RWActiveLightListCount;
RWStructuredBuffer<uint> RWActiveLightListBuffer;
StructuredBuffer<uint> ActiveLightListCount;
StructuredBuffer<uint> ActiveLightListBuffer;


StructuredBuffer<uint> LightGrid_ListLightIndexBuffer;
StructuredBuffer<uint> LightGrid_GridLightListOffsetBuffer;
StructuredBuffer<float> LightGrid_GridLightListCdfBuffer;
StructuredBuffer<uint> LightGrid_GridLightListLengthBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
StructuredBuffer<uint4> LightGrid_BloomFilterBuffer;

RWStructuredBuffer<uint> RWLightGrid_ListAllocatorBuffer;

RWStructuredBuffer<uint> RWLightGrid_ListLightIndexBuffer;
RWStructuredBuffer<float> RWLightGrid_GridLightListCdfBuffer;
RWStructuredBuffer<uint> RWLightGrid_GridLightListOffsetBuffer;
RWStructuredBuffer<uint> RWLightGrid_GridLightListLengthBuffer;
RWStructuredBuffer<uint> RWLightGrid_BloomFilterBuffer;


// Top level AS
RaytracingAccelerationStructure TLAS;

#ifndef TILE_SIZE
// Defaults to a smaller tile size for better thread coherency
#define TILE_SIZE 8
#endif

#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE 128
#endif

#ifndef WAVE_SIZE
#define WAVE_SIZE 32
#endif


struct SurfaceRayPayload {
    float TCurrent;
    uint HitInstanceCustomIndex;
    uint HitGeometryIndex;
    uint HitPrimitiveIndex;
    float2 HitBarycentrics;
};


struct VolumeRayPayload {
    uint Seed;
    float TSampled;
    float Transmittance;
    float3 Albedo;
};

RWTexture2D<float4> RWRadiance; // Output radiance (1spp)


[shader("raygeneration")]
void ReferencePathTracerRaygen() {

    uint2 RayIndex = DispatchRaysIndex().xy;
    uint2 DispatchSize = DispatchRaysDimensions().xy;

    RayDesc Ray = (RayDesc)0;
    // Spawn camera ray
    {
        CameraParameters C = GetActiveCamera();
        Ray.Origin = C.Position;
        float2 UV = ((float2)RayIndex + 0.5f.xx) / (float2)DispatchSize;
        float2 NDC2 = UVToNDC2(UV);
        Ray.Direction = NDC2ToCameraDirectionUnnormalized(C, NDC2);
        float DirLen = length(Ray.Direction);
        Ray.Direction = Ray.Direction / DirLen; // Normalize direction
        Ray.TMin = C.NearPlane * DirLen;
        Ray.TMax = C.FarPlane * DirLen; // Scale TMin and TMax by the direction length
    }

    Random rng = MakeRandom(61937141 + RayIndex.x * 6183 + RayIndex.y);

    // Initialize trace state
    float3 Radiance = 0;
    float3 Throughput = 1;
    uint BounceIndex = 0;

    
    while(true) {
        SurfaceRayPayload SurfacePayload = (SurfaceRayPayload)0;
        // Trace surface ray first
        TraceRay(
            TLAS,
            RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            0xFF, // Ray mask
            0,    // Suface ray
            0,    // SBT stride
            0,    // Miss shader index
            Ray,
            SurfacePayload
        );
        bool bSurfaceHit = SurfacePayload.TCurrent < Ray.TMax;
        // Clamp TMax 
        Ray.TMax = min(Ray.TMax, SurfacePayload.TCurrent);
        VolumeRayPayload VolumePayload = (VolumeRayPayload)0;
        {
            VolumePayload.Transmittance = 1;
            VolumePayload.TSampled = Infinity;
        }
        // Trace volume ray
        TraceRay(
            TLAS,
            0, // We must keep both side hits of volumes (because the trace may start within volumes).
            0xFF, // Ray mask
            1,    // Volume ray
            0,    // SBT stride
            0,    // Miss shader index
            Ray,
            VolumePayload
        );
        // Check if we have a closer volume hit
        if(VolumePayload.TSampled < Ray.TMax) {
            // Volume hit is closer, spawn volume intersection
            Ray.Origin = Ray.Origin + Ray.Direction * VolumePayload.TSampled;
            Ray.TMin = 1e-4f;
            Ray.TMax = Infinity;
            // Sample outgoing direction
            float Pdf = 0;
            Ray.Direction = SampleHenyeyGreenstein(0.5, rng.rand2(), Pdf); // Perfect sampling
            Throughput *= VolumePayload.Albedo;
        } else if(bSurfaceHit) {
            // Surface hit
            // Extract intersection
            float4 AlbedoOpacity = 0;
            float3 Normal = 0;
            float3 Emission = 0;
            float2 MetallicRoughness = 0;
            {
                
            }
        } else {
            // No hits, accumulate environment lighting and terminate
            float3 RayDirection = Ray.Direction;
            float3 EnvironmentColor = EnvironmentMap.SampleLevel(LinearSampler, -RayDirection, 0).xyz;
            Radiance += Throughput * EnvironmentColor;
            break;
        }
        // Shade intersection and accumulate radiance
        {
            float3 OutEmission = 0, OutDirectIllumination = 0; 
            // TODO MIS
            // BSDF / PDF
            float3 BSDF_Pdf = ShadeIntersction(Payload, Ray, OutDirectIllumination, OutEmission);
            Radiance += OutDirectIllumination * Throughput + OutEmission * Throughput;
            Throughput *= BSDF_Pdf;
        }
        // Russian roulette
        if(BounceIndex >= 2) {
            float U = rng.rand();
            float ContinuationProbability = min(hmin(Throughput), 0.95f);
            if(U > ContinuationProbability) {
                break;
            }
            Throughput = Throughput * (1.0f / max(ContinuationProbability, 5e-3f));
        }
        BounceIndex ++;
    }
    RWRadiance[RayIndex] = float4(Radiance, 1.0f);
}

[shader("miss")]
void ReferencePathTracerMiss(inout RayPayload Payload: SV_RayPayload) {
    Payload.HitInstanceCustomIndex = 0xFFFFFFFF;
}

DefaultStaticMeshVertex InterpolateVertex(DefaultStaticMeshVertex C, DefaultStaticMeshVertex A, DefaultStaticMeshVertex B, float2 Barycentric) {
    DefaultStaticMeshVertex Result;
    float Z = (1 - Barycentric.x - Barycentric.y);
    Result.Position = A.Position * Barycentric.x + B.Position * Barycentric.y + C.Position * Z;
    Result.Normal = normalize(A.Normal * Barycentric.x + B.Normal * Barycentric.y + C.Normal * Z);
    Result.UV = A.UV * Barycentric.x + B.UV * Barycentric.y + C.UV * Z;
    return Result;
}

[shader("anyhit")]
void ReferencePathTracerAnyHit(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    // Always hit.
}

[shader("closesthit")]
void ReferencePathTracerClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    // Pack hit data to payload
    Payload.HitInstanceCustomIndex = InstanceCustomIndex;
    Payload.HitGeometryIndex = DescriptionIndex;
    Payload.HitPrimitiveIndex = Triangle;
    Payload.HitBarycentrics = Attributes.barycentrics;
    Payload.bBackfaceHit = HitKind() & HIT_KIND_TRIANGLE_BACK_FACE;
}