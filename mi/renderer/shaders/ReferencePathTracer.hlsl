#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedLight.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Light.hlsl"
#include "headers/VolumePrimitive.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "headers/Scattering.hlsl"
#include "headers/MaterialEvaluation.hlsl"

#include "resources/BindlessTextureResources.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"
#include "resources/EnvironmentLightResource.hlsl"

// All area lights
StructuredBuffer<AreaLight> LightBuffer;
StructuredBuffer<PackedPrecomputedLight> LightGrid_PrecomputedActiveLightBuffer;

StructuredBuffer<uint> LightGrid_ActiveLightListCount;
StructuredBuffer<uint> LightGrid_ActiveLightListBuffer;


StructuredBuffer<uint> LightGrid_ListLightIndexBuffer;
StructuredBuffer<uint> LightGrid_GridLightListOffsetBuffer;
StructuredBuffer<float> LightGrid_GridLightListCdfBuffer;
StructuredBuffer<uint> LightGrid_GridLightListLengthBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
StructuredBuffer<uint4> LightGrid_BloomFilterBuffer;

StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;

// Top level AS
RaytracingAccelerationStructure TLAS;

struct ReferencePathTracerUB {
    uint FrameIndex;
    uint EnableAccumulation;
    uint MaxNumBounces;
    uint Padding1;
};

ConstantBuffer<ReferencePathTracerUB> UB;


struct [raypayload] RayPayload {
    bool bIsSurfaceHit; // True if hit a surface, false if miss or hit a volume
    bool bIsFrontFace;
    float TCurrent;
    uint HitInstanceCustomIndex;
    uint HitGeometryIndex;
    uint HitPrimitiveIndex;
    float2 HitBarycentrics;
};

[[vk::image_format("rgba32f")]]
RWTexture2D<float4> RWRadiance; // Output radiance (1spp)

#define MAX_OVERLAPPING_VOLUME_PRIMITIVES 16

float ResampleVolumePrimitives (
    RayDesc Ray,
    uint InstanceIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES],
    uint VolumePrimitiveIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES],
    uint NumVolumePrimitives,
    inout Random rng,
    out float3 OutSampledColor
) {
    float SampledDistance = Infinity;
    OutSampledColor = 0;
    [unroll(MAX_OVERLAPPING_VOLUME_PRIMITIVES)]
    for(int i = 0; i < min(NumVolumePrimitives, MAX_OVERLAPPING_VOLUME_PRIMITIVES); i++) {
        VolumePrimitive Primitive = UnpackVolumePrimitive(PrimitiveData[VolumePrimitiveIndices[i]]);
        float3x4 ToObject = RenderableInverseTransformBuffer[InstanceIndices[i]];
        float2 lr = 0;
        float Dist = 0;
        bool bIntersected = RayIntersect(Ray.Origin, Ray.Direction, Primitive, ToObject, lr, Dist);
        if(bIntersected) {
            float TMin = Ray.TMin;
            lr.x = max(lr.x, TMin);
            lr.y = max(lr.y, TMin);
            RayVolumePrimitiveIntersection Distr = (RayVolumePrimitiveIntersection)0;
            Distr.l = lr.x;
            Distr.r = lr.y;
            Distr.Density = Primitive.Opacity * VolumePrimitiveRayDecay(Dist);
            Distr.Color   = Primitive.Color;
            // Make a volume sample
            float Distance = SampleRayVolumePrimitiveIntersection(Distr, rng.rand());
            // Compare with current sample
            if(Distance < SampledDistance) {
                // Pick the closer one
                SampledDistance = Distance;
                OutSampledColor = Primitive.Color;
            }
        }
    }
    return SampledDistance;
}

[shader("raygeneration")]
void ReferencePathTracerRaygen() {

    uint2 RayIndex = DispatchRaysIndex().xy;
    uint2 DispatchSize = DispatchRaysDimensions().xy;

    // TODO variable g
    float g = 0.f;

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

    Random rng = MakeRandom(61937141 + (RayIndex.x * 6183 + RayIndex.y) * UB.FrameIndex);

    // Initialize trace state
    float3 Radiance = 0;
    float3 Throughput = 1;
    uint BounceIndex = 0;

    // Current participating medium
    uint   OverlappingVolumePrimitivesInstanceIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES];
    uint   OverlappingVolumePrimitiveIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES];
    uint   CurrentOverlappingVolumePrimitiveCount = 0;
    float3 VolumeSampledColor = 0;

    uint MaxNumBounces = UB.MaxNumBounces;
    
    while(BounceIndex < MaxNumBounces) {
        RayPayload Payload = (RayPayload)0;
        Payload.TCurrent = Ray.TMax;
        // Trace surface ray first
        TraceRay(
            TLAS,
            0,
            0xFF, // Ray mask
            0,    // Suface ray
            0,    // SBT stride
            0,    // Miss shader index
            Ray,
            Payload
        );
        bool bScatter = false;
        if(Payload.TCurrent >= Ray.TMax) // Hits nothing, stepping over the volume sample
        {
            if(CurrentOverlappingVolumePrimitiveCount == 0) {
                // No pending volume hit is present, accumulate environment lighting and terminate
                float3 EnvironmentColor = EvaluateEnvironmentMap(-Ray.Direction);
                Radiance += Throughput * EnvironmentColor;
                // Terminate directly
                break;
            } else {
                // Volume intersection detected.
                // Ray hit a surface or hits nothing, overpassing the current volume sample.
                // Scatter the ray at the current volume sample position.
                float Pdf;
                float3 LocalSampledDirection = SampleHenyeyGreenstein(g, rng.rand2(), Pdf);
                // We do not need to take account of the value and pdf because of perfect sampling
                
                // Forward and restart the ray
                Ray.Origin = Ray.Origin + Ray.Direction * Ray.TMax;
                float3 Tangent, Bitangent;
                GetOrthoVectors(Ray.Direction, Tangent, Bitangent);
                Ray.Direction = 
                    LocalSampledDirection.x * Tangent 
                    + LocalSampledDirection.y * Bitangent
                    + LocalSampledDirection.z * Ray.Direction;
                Ray.TMin = 1e-4f;

                // Phase function cancelled out naturally due to perfect sampling
                Throughput *= VolumeSampledColor;

                // Spawn new volume sample, trace till volume hit.
                Ray.TMax = ResampleVolumePrimitives(
                    Ray,
                    OverlappingVolumePrimitivesInstanceIndices,
                    OverlappingVolumePrimitiveIndices,
                    CurrentOverlappingVolumePrimitiveCount,
                    rng,
                    VolumeSampledColor
                );

                // This is a scattering event;
                bScatter = true;
            }
        } else if(Payload.bIsSurfaceHit) { // Hits a mesh surface before pending volume scattering / miss
            // Second case: Surface hit (ray hit a surface before passing through the volume sample).
            if(Payload.bIsFrontFace) {
                // The surface hit is closer than the volume hit. Spawn a surface hit
                
                uint InstanceIndex = Payload.HitInstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
                // Extract intersection
                IntersectionMaterial Intersection =  EvaluateStaticMeshRenderableIntersectionMaterial(
                    InstanceIndex,
                    Payload.HitGeometryIndex,
                    Payload.HitPrimitiveIndex,
                    Payload.HitBarycentrics,
                    0
                );
                if(Intersection.Opacity > rng.rand()) {
                    // Sample outgoing ray direction
                    ShadingMaterial M = GetShadingMaterial(Intersection);
                    // if(dot(M.Normal, Ray.Direction) > 0) M.Normal = -M.Normal;
                    float3 SampledDirection;
                    float Pdf = SampleBDSF(M, -Ray.Direction, rng.rand2(), SampledDirection);
                    
                    // Update ray
                    Ray.Origin = Intersection.WorldPosition + Intersection.GeometryNormal * 2e-5f;
                    Ray.Direction = SampledDirection;
                    Ray.TMin = 1e-4f;

                    // Accumulate radiance
                    Radiance += Intersection.Emission * Throughput;

                    // Update throughput
                    Throughput *= 
                        EvaluateBSDF(M, -Ray.Direction, SampledDirection)
                        * saturate(dot(M.Normal, SampledDirection)) / max(Pdf, 1e-5f);

                    // Spawn new volume sample
                    Ray.TMax = ResampleVolumePrimitives(
                        Ray,
                        OverlappingVolumePrimitivesInstanceIndices,
                        OverlappingVolumePrimitiveIndices,
                        CurrentOverlappingVolumePrimitiveCount,
                        rng,
                        VolumeSampledColor
                    );

                    // This is a scattring event
                    bScatter = true;
                } else {
                    // Transparent surface, advance ray to next hit
                    Ray.TMin = Payload.TCurrent + 1e-5f;
                }
            } else {
                // Ignore backface hits for surfaces
                // Advance ray to next hit
                Ray.TMin = Payload.TCurrent + 1e-5f;
            }
        } else { // Hit a volume primitive boundary before pending volume scattering / miss
            // Fetch the volume primitive
            // Get the index of the volume primitive (each volume primitive have 20 triangles for proxy geometry)
            uint InstanceVolPrimitiveIndex = Payload.HitPrimitiveIndex / 20;
            uint InstanceIndex = Payload.HitInstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
            VolumePrimitivesInstanceHeader Renderable = GetVolumePrimitivesInstanceHeader(RenderableHeaderBuffer[InstanceIndex]);
            uint VolPrimitiveOffset = VolumePrimitivesHeaderBuffer[Renderable.VolumePrimitivesIndex].PrimitiveOffset;
            uint PrimitiveIndex = VolPrimitiveOffset + InstanceVolPrimitiveIndex;
            VolumePrimitive Primitive = UnpackVolumePrimitive(PrimitiveData[PrimitiveIndex]);
            float3x4 ToObject = RenderableInverseTransformBuffer[InstanceIndex];
            if(!Payload.bIsFrontFace) {
                // Backface hits: entering the volume. Spawn a volume sample for the primitive.
                float2 lr = 0;
                float Dist = 0;
                bool bIntersected = RayIntersect(Ray.Origin, Ray.Direction, Primitive, ToObject, lr, Dist);
                if(bIntersected) {
                    float TMin = Ray.TMin;
                    lr.x = max(lr.x, TMin);
                    lr.y = max(lr.y, TMin);
                    RayVolumePrimitiveIntersection Distr = (RayVolumePrimitiveIntersection)0;
                    Distr.l = lr.x;
                    Distr.r = lr.y;
                    Distr.Density = Primitive.Opacity * VolumePrimitiveRayDecay(Dist);
                    Distr.Color   = Primitive.Color;
                    // Make a volume sample
                    float Distance = SampleRayVolumePrimitiveIntersection(Distr, rng.rand());
                    // Compare with the pending volume hit
                    if(Distance < Ray.TMax) {
                        // Pick the closer one
                        Ray.TMax = Distance;
                        VolumeSampledColor = Primitive.Color;
                    }
                }
                // Insert to the list
                if(CurrentOverlappingVolumePrimitiveCount < MAX_OVERLAPPING_VOLUME_PRIMITIVES) {
                    OverlappingVolumePrimitiveIndices[CurrentOverlappingVolumePrimitiveCount] = PrimitiveIndex;
                    OverlappingVolumePrimitivesInstanceIndices[CurrentOverlappingVolumePrimitiveCount] = InstanceIndex;
                    CurrentOverlappingVolumePrimitiveCount ++;
                }
            } else {
                // Frontface hits: exiting the volume.
                // Remove the primitive from the overlapping list
                // Here we make an approximation that the primitive boudary is perfectly the proxy geometry
                // when sampling next volume hits aroud surfaces. The overhead introduced for precisely 
                // tracking and sampling the volume is too high. 
                bool bFound = false;
                for(int i = 0; i < min(CurrentOverlappingVolumePrimitiveCount, MAX_OVERLAPPING_VOLUME_PRIMITIVES); i++) {
                    bool bIsCurrentOne = 
                        (OverlappingVolumePrimitiveIndices[i] == PrimitiveIndex)
                        && (OverlappingVolumePrimitivesInstanceIndices[i] == InstanceIndex);
                    bFound |= bIsCurrentOne;
                    if(bFound && i < MAX_OVERLAPPING_VOLUME_PRIMITIVES - 1) {
                        // Overwrite with the next element
                        OverlappingVolumePrimitiveIndices[i] = OverlappingVolumePrimitiveIndices[i + 1];
                        OverlappingVolumePrimitivesInstanceIndices[i] = OverlappingVolumePrimitivesInstanceIndices[i + 1];
                    }
                }
                if(bFound) {
                    CurrentOverlappingVolumePrimitiveCount --;
                }
            }
            // Forward the ray a little bit
            Ray.TMin = min(Payload.TCurrent + 2e-5f, Ray.TMax);
        }

        if(bScatter) {
            BounceIndex ++;
            // Russian roulette
            if(BounceIndex >= 2) {
                float U = rng.rand();
                float ContinuationProbability = min(RadianceToLuminance(Throughput), 0.95f);
                if(U > ContinuationProbability) {
                    break;
                }
                Throughput = Throughput * (1.0f / max(ContinuationProbability, 5e-3f));
            }
        }
    }
    if(UB.EnableAccumulation != 0) {
        float4 FilmRadiance = RWRadiance[RayIndex];
        if (any(isnan(FilmRadiance))) FilmRadiance = 0;
        FilmRadiance.w = min(FilmRadiance.w + 1.0f, 32768.0f);
        float InvSampleCount = 1.0f / max(FilmRadiance.w, 1.f);
        if (any(isnan(Radiance))) Radiance = 0;
        FilmRadiance.rgb = (1.f - InvSampleCount) * FilmRadiance.rgb + InvSampleCount * Radiance;
        RWRadiance[RayIndex] = FilmRadiance;
    } else {
        RWRadiance[RayIndex] = float4(Radiance, 1.0f);
    }
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
    Payload.TCurrent = RayTCurrent();
    Payload.HitInstanceCustomIndex = InstanceCustomIndex;
    Payload.HitGeometryIndex = DescriptionIndex;
    Payload.HitPrimitiveIndex = Triangle;
    Payload.HitBarycentrics = Attributes.barycentrics;
    Payload.bIsFrontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    Payload.bIsSurfaceHit = (InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK) == 0;
}