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
#include "headers/Scattering.hlsl"
#include "headers/MaterialEvaluation.hlsl"

#include "resources/BindlessTextureResources.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"

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

Texture2D<float4> EnvironmentMap;

StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;


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

struct RayPayload {
    bool bIsSurfaceHit; // True if hit a surface, false if miss or hit a volume
    bool bIsFrontFace;
    float TCurrent;
    uint HitInstanceCustomIndex;
    uint HitGeometryIndex;
    uint HitPrimitiveIndex;
    float2 HitBarycentrics;
};

RWTexture2D<float4> RWRadiance; // Output radiance (1spp)

[shader("raygeneration")]
void ReferencePathTracerRaygen() {

    uint2 RayIndex = DispatchRaysIndex().xy;
    uint2 DispatchSize = DispatchRaysDimensions().xy;

    RayDesc Ray = (RayDesc)0;
    CameraParameters C = GetActiveCamera();
    // Spawn camera ray
    {
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

    // Current participating medium
#define MAX_OVERLAPPING_VOLUME_PRIMITIVES 16
    uint   OverlappingVolumePrimitivesInstanceIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES];
    uint   OverlappingVolumePrimitiveIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES];
    uint   CurrentOverlappingVolumePrimitiveCount = 0;
    float  VolumeSampledRayDistance = Infinity;
    float3 VolumeSampledColor = 0;
    
    while(BounceIndex < 16) {
        RayPayload Payload = (RayPayload)0;
        Payload.TCurrent = Ray.TMax;
        // Trace surface ray first
        TraceRay(
            TLAS,
            RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            0xFF, // Ray mask
            0,    // Suface ray
            0,    // SBT stride
            0,    // Miss shader index
            Ray,
            Payload
        );
        // First case: miss witout volume intersection
        if(Payload.TCurrent >= Ray.TMax && Payload.TCurrent < VolumeSampledRayDistance) {
            // No hits, accumulate environment lighting and terminate
            float3 RayDirection = Ray.Direction;
            float3 EnvironmentColor = EnvironmentMap.SampleLevel(LinearWrapSampler, -RayDirection, 0).xyz;
            Radiance += Throughput * EnvironmentColor;
            break;
        } else if(Payload.bIsSurfaceHit && Payload.TCurrent < VolumeSampledRayDistance) {
            // Surface hit.
            if(Payload.bIsFrontFace) {
                // The surface hit is closer than the volume hit. Spawn a surface hit
                
                uint InstanceIndex = Payload.HitInstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
                // Extract intersection
                IntersectionMaterial Intersection =  EvaluateStaticMeshRenderableIntersectionMaterial(
                    InstanceIndex,
                    Payload.HitGeometryIndex,
                    Payload.HitPrimitiveIndex,
                    Payload.HitBarycentrics
                );
                // Sample outgoing ray direction
                ShadingMaterial M = GetShadingMaterial(Intersection);
                float3 SampledDirection;
                float Pdf = SampleBDSF(M, -Ray.Direction, rng.rand2(), SampledDirection);
                
                // Update ray
                Ray.Origin = Intersection.WorldPosition + Intersection.Normal * 2e-5f;
                Ray.Direction = SampledDirection;
                Ray.TMin = 1e-4f;
                Ray.TMax = C.FarPlane;

                // Accumulate radiance
                Radiance += Intersection.Emission * Throughput;

                // Update throughput
                Throughput *= EvaluateBSDF(M, -Ray.Direction, SampledDirection) * abs(dot(M.Normal, SampledDirection)) / max(Pdf, 1e-5f);

                BounceIndex ++;
            } else {
                // Ignore backface hits for surfaces
                // Advance ray to next hit
                Ray.TMin = Payload.TCurrent + 1e-4f;
            }
        } else {
            // Volume hit or volume intersection.
            if(Payload.bIsSurfaceHit || Payload.TCurrent >= Ray.TMax) {
                // Volume intersection detected.
                // Scatter the ray at the current volume sample position.
                float Pdf;
                float3 LocalSampledDirection = SampleHenyeyGreenstein(0.5, rng.rand2(), Pdf);
                // We do not need to take account of the value and pdf because of perfect sampling
                
                // Forward the ray
                Ray.Origin = Ray.Origin + Ray.Direction * VolumeSampledRayDistance;
                float3 Tangent, Bitangent;
                GetOrthoVectors(Ray.Direction, Tangent, Bitangent);
                Ray.Direction = 
                    LocalSampledDirection.x * Tangent 
                  + LocalSampledDirection.y * Bitangent
                  + LocalSampledDirection.z * Ray.Direction;
                Ray.TMin = 1e-4f;
                Ray.TMax = C.FarPlane;

                Throughput *= VolumeSampledColor;
                BounceIndex ++;
            } else {
                // Volume boundary hit.
                // Fetch the volume primitive
                // Get the index of the volume primitive (each volume primitive have 20 triangles for proxy geometry)
                uint InstanceVolPrimitiveIndex = Payload.HitPrimitiveIndex / 20;
                uint InstanceIndex = Payload.HitInstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
                VolumePrimitivesInstanceHeader Renderable = GetVolumePrimitivesInstanceHeader(RenderableHeaderBuffer[InstanceIndex]);
                uint VolPrimitiveOffset = VolumePrimitivesHeaderBuffer[Renderable.VolumePrimitivesIndex].PrimitiveOffset;
                uint PrimitiveIndex = VolPrimitiveOffset + InstanceVolPrimitiveIndex;
                VolumePrimitive Primitive = UnpackVolumePrimitive(PrimitiveData[PrimitiveIndex]);
                if(!Payload.bIsFrontFace) {
                    // Backface hits: entering the volume. Spawn a volume sample for the primitive.
                    float3x4 ToObject = RenderableTransformBuffer[InstanceIndex];
                    float2 lr = 0;
                    float Dist = 0;
                    bool bIntersected = RayIntersect(Ray.Origin, Ray.Direction, Primitive, ToObject, lr, Dist);
                    if(bIntersected) {
                        float TMin = RayTMin();
                        lr.x = max(lr.x, TMin);
                        lr.y = max(lr.y, TMin);
                        RayVolumeDistribution Distr = (RayVolumeDistribution)0;
                        Distr.l = lr.x;
                        Distr.r = lr.y;
                        Distr.Density = Primitive.Opacity;
                        Distr.Color   = Primitive.Color;
                        // Make a volume sample
                        float Distance = SampleRayVolumeDistribution(Distr, rng.rand());
                        // Compare with VolumeSampledRayDistance
                        if(Distance < VolumeSampledRayDistance) {
                            // Pick the closer one
                            VolumeSampledRayDistance = Distance;
                            VolumeSampledColor = Primitive.Color;
                        }
                    }
                    // Insert to the list
                    OverlappingVolumePrimitiveIndices[CurrentOverlappingVolumePrimitiveCount] = PrimitiveIndex;
                    OverlappingVolumePrimitivesInstanceIndices[CurrentOverlappingVolumePrimitiveCount] = InstanceIndex;
                    CurrentOverlappingVolumePrimitiveCount ++;
                } else {
                    // Frontface hits: exiting the volume.
                    // Remove the primitive from the overlapping list
                    // Here we make an approximation that the primitive boudary is perfectly the proxy geometry
                    // when sampling next volume hits aroud surfaces. The overhead introduced for precisely 
                    // tracking and sampling the volume is too high. 
                    bool bFound = false;
                    for(int i = 0; i < CurrentOverlappingVolumePrimitiveCount; i ++) {
                        bFound |= 
                            (OverlappingVolumePrimitiveIndices[i] == PrimitiveIndex)
                         && (OverlappingVolumePrimitiveIndices[i] == InstanceIndex);
                        if(bFound) {
                            // Overwrite with the next element
                            OverlappingVolumePrimitiveIndices[i] = OverlappingVolumePrimitiveIndices[i + 1];
                            OverlappingVolumePrimitivesInstanceIndices[i] = OverlappingVolumePrimitivesInstanceIndices[i + 1];
                        }
                    }
                    CurrentOverlappingVolumePrimitiveCount --;
                }
                // Forward the ray a little bit
                Ray.TMin = Payload.TCurrent + 2e-5f;
            }
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
    Payload.bIsFrontFace = !(HitKind() & HIT_KIND_TRIANGLE_BACK_FACE);
    Payload.bIsSurfaceHit = (InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK) == 0;
}