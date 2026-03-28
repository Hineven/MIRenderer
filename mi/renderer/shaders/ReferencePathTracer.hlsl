#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedLight.hlsl"
#include "shared/SharedVolumeGrid.hlsl"
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
#include "headers/VolumeGridLib.hlsl"
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
StructuredBuffer<VolumeGridHeader> VolumeGridHeaderBuffer;

// Top level AS
RaytracingAccelerationStructure TLAS;

struct ReferencePathTracerUB {
    uint FrameIndex;
    uint EnableAccumulation;
    uint MaxNumBounces;
    float EnvironmentMapLOD;
    float3 EnvironmentMapMultiplier;
    uint Padding; // Padding to make the size of the struct a multiple of 16 bytes
};

ConstantBuffer<ReferencePathTracerUB> UB;


struct [raypayload] RayPayload {
    bool bIsSurfaceHit; // True if hit a surface, false if miss or hit a volume
    bool bIsVolumeGridHit; // True if hit a volume grid boundary
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
#define MAX_OVERLAPPING_VOLUME_GRIDS 16

// Remove Volume Primitive from Overlapping Volume Primitive List.
void RemoveVolumePrimitive(
    uint PrimitiveIndex,
    uint InstanceIndex,
    inout uint Count,
    inout uint VolumePrimitiveIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES],
    inout uint InstanceIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES]
) {
    bool bFound = false;
    for (int i = 0; i < min(Count, MAX_OVERLAPPING_VOLUME_PRIMITIVES); i++)
    {
        if ((!bFound) && (VolumePrimitiveIndices[i] == PrimitiveIndex) && (InstanceIndices[i] == InstanceIndex))
        {
            bFound = true;
        }
        if (bFound && i < MAX_OVERLAPPING_VOLUME_PRIMITIVES - 1)
        {
            VolumePrimitiveIndices[i] = VolumePrimitiveIndices[i + 1];
            InstanceIndices[i] = InstanceIndices[i + 1];
        }
    }
    if (bFound && Count > 0)
    {
        Count--;
    }
}

// Remove Volume Grid from Overlapping Volume Gird List.
void RemoveVolumeGrid(
    uint GridIndex,
    uint InstanceIndex,
    inout uint Count,
    inout uint VolumeGridIndices[MAX_OVERLAPPING_VOLUME_GRIDS],
    inout uint InstanceIndices[MAX_OVERLAPPING_VOLUME_GRIDS]
) {
    bool bFound = false;
    for (int i = 0; i < min(Count, MAX_OVERLAPPING_VOLUME_GRIDS); i++)
    {
        if ((!bFound) && (VolumeGridIndices[i] == GridIndex) && (InstanceIndices[i] == InstanceIndex))
        {
            bFound = true;
        }
        if (bFound && i < MAX_OVERLAPPING_VOLUME_GRIDS - 1)
        {
            VolumeGridIndices[i] = VolumeGridIndices[i + 1];
            InstanceIndices[i] = InstanceIndices[i + 1];
        }
    }
    if(bFound && Count > 0)
    {
        Count--;
    }
}

// Sample a Possible Scatter Position in a List of VolumePrimitive.
// Used when Ray's Start Position is in Volume Primitives.
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

// Delta Tracking for Volume Grid
// Returns the distance to the scattering event, or Infinity if passed through.
float ResampleVolumeGrids(
    RayDesc Ray,
    uint InstanceIndices[MAX_OVERLAPPING_VOLUME_GRIDS],
    uint VolumeGridIndices[MAX_OVERLAPPING_VOLUME_GRIDS],
    uint NumVolumeGrids,
    inout Random rng,
    out float3 OutSampledColor
) {
    float SampledDistance = Infinity;
    OutSampledColor = float3(0, 0, 0);

    // Translate all the grids where the current ray is located.
    [unroll(MAX_OVERLAPPING_VOLUME_GRIDS)]
    for (int i = 0; i < min(NumVolumeGrids, MAX_OVERLAPPING_VOLUME_GRIDS); i++)
    {
        uint InstanceIndex = InstanceIndices[i];
        uint VolumeGridIndex = VolumeGridIndices[i];
        VolumeGridHeader Header = VolumeGridHeaderBuffer[VolumeGridIndex];

        // 1. Calculate path of ray in object space
        float3x4 WorldToObj = RenderableInverseTransformBuffer[InstanceIndex];
        float3 RayOriginObj = mul(WorldToObj, float4(Ray.Origin, 1.0));
        float3 RayDirObj = mul((float3x3) WorldToObj, Ray.Direction);

        // 2. Intersect AABB to find entry/exit
        // Get AABB Size.
        float3 BoxMin = Header.LocalMin;
        float3 BoxMax = Header.LocalMax;
        float3 BoxSize = max(BoxMax - BoxMin, 1e-6f);
        float3 InvBoxSize = 1.0f / BoxSize;

        float tNear, tFar;
        bool bHit = IntersectAABB(RayOriginObj, RayDirObj, BoxMin, BoxMax, tNear, tFar);
        if (!bHit)
        {
            continue;
        }

        float tStart = max(0.0f, tNear);
        float tEnd = max(0.0f, tFar);
        if (tStart >= tEnd)
        {
            continue;
        }

        // 3. Get Bindless Texture
        Texture3D<float4> VolumeTex = GetBindlessVolumeSRV(Header.TextureBindlessIndex);

        // 4. Calculate Majorant (Max Density) for this ray segment
        float3 RayOriginNorm = (RayOriginObj - BoxMin) * InvBoxSize;
        float3 RayDirNorm = RayDirObj * InvBoxSize;

        float MaxDensity = CalculateMaxDensityDDA(
            VolumeTex,
            RayOriginNorm,
            RayDirNorm,
            tStart,
            tEnd,
            1.0f
        );

        MaxDensity = max(MaxDensity, 1e-6f);
        float InvMaxDensity = 1.0f / MaxDensity;

        // 5. Delta Tracking Loop
        float tCurrent = tStart;

        while (true)
        {
            // Free flight step
            tCurrent -= log(max(1e-6f, rng.rand())) * InvMaxDensity;

            if (tCurrent >= tEnd)
            {
                // Pass through the volume
                break;
            }

            // Sample Density at current position
            float3 UVW = RayOriginNorm + RayDirNorm * tCurrent;
            float4 TexVal = VolumeTex.SampleLevel(PointWrapSampler, UVW, 0);
            float Density = TexVal.a;

            // Null collision rejection
            if (rng.rand() < (Density * InvMaxDensity))
            {
                // Choose the closest sampled position
                if (tCurrent < SampledDistance)
                {
                    SampledDistance = tCurrent;
                    OutSampledColor = TexVal.rgb;
                }
                break; // Process next grid
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
        Ray.TMin = 0;
        Ray.TMax = Infinity;
    }

    Random rng = MakeRandom(61937141 + (RayIndex.x * 6183 + RayIndex.y) * UB.FrameIndex);

    // Initialize trace state
    float3 Radiance = 0;
    float3 Throughput = 1;
    uint BounceIndex = 0;

    // --- State: Primitives ---
    uint OverlappingVolumePrimitivesInstanceIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES];
    uint OverlappingVolumePrimitiveIndices[MAX_OVERLAPPING_VOLUME_PRIMITIVES];
    uint CurrentOverlappingVolumePrimitiveCount = 0;

    // --- State: Grids ---
    uint OverlappingVolumeGridsInstanceIndices[MAX_OVERLAPPING_VOLUME_GRIDS];
    uint OverlappingVolumeGridIndices[MAX_OVERLAPPING_VOLUME_GRIDS];
    uint CurrentOverlappingVolumeGridCount = 0;

    float3 VolumeSampledColor = 0;
    uint MaxNumBounces = UB.MaxNumBounces;
    
    while (BounceIndex < MaxNumBounces)
    {

        // ------------------------------
        // Step 1: Free-path Sampling
        // ------------------------------
        float T_VolumeScatter = Infinity;
        float3 TempColor = float3(0, 0, 0);

        // 1. Sample Volume Primitives
        if (CurrentOverlappingVolumePrimitiveCount > 0)
        {
            float t = ResampleVolumePrimitives(
                Ray,
                OverlappingVolumePrimitivesInstanceIndices,
                OverlappingVolumePrimitiveIndices,
                CurrentOverlappingVolumePrimitiveCount,
                rng,
                TempColor
            );
            if (t < T_VolumeScatter)
            {
                T_VolumeScatter = t;
                VolumeSampledColor = TempColor;
            }
        }

        // 2. Sample Volume Grids
        if (CurrentOverlappingVolumeGridCount > 0)
        {
            float t = ResampleVolumeGrids(
                Ray,
                OverlappingVolumeGridsInstanceIndices,
                OverlappingVolumeGridIndices,
                CurrentOverlappingVolumeGridCount,
                rng,
                TempColor
            );
            if (t < T_VolumeScatter)
            {
                T_VolumeScatter = t;
                VolumeSampledColor = TempColor;
            }
        }

        // --------------------------
        // Step 2: Geometry Trace
        // --------------------------
        // We want to know which happens first: Reach static mesh, or oveerlapping volume primitives
        Ray.TMax = T_VolumeScatter;

        RayPayload Payload = (RayPayload) 0;
        Payload.TCurrent = Ray.TMax;
        Payload.bIsSurfaceHit = false;
        Payload.bIsVolumeGridHit = false;

        // Trace surface ray first
        TraceRay(
            TLAS,
            0,
            0xFF, // Ray mask
            0, // Suface ray
            0, // SBT stride
            0, // Miss shader index
            Ray,
            Payload
        );

        // ----------------------------------------------
        // Step 3: Decide which case should be chosen
        // ----------------------------------------------

        // Case A: Overlapping Volume Primitives Scatter
        if (T_VolumeScatter < Infinity && Payload.TCurrent >= T_VolumeScatter)
        {

            // 1. Update ray throughtput. Phase function cancelled out naturally due to perfect sampling
            Throughput *= VolumeSampledColor;

            // 2. Sample ray scatter direction
            // In fact, wo do not need to tak account of the value and pdf because of perfect sampling.
            float Pdf;
            float3 LocalSampledDirection = SampleHenyeyGreenstein(g, rng.rand2(), Pdf);

            // Rebuild orthogonal space.
            float3 Tangent, Bitangent;
            GetOrthoVectors(Ray.Direction, Tangent, Bitangent);
            Ray.Direction = normalize(
                LocalSampledDirection.x * Tangent
                + LocalSampledDirection.y * Bitangent
                + LocalSampledDirection.z * Ray.Direction
            );

            // 3. Forward the ray ray origin to scatter position
            Ray.Origin = Ray.Origin + Ray.Direction * T_VolumeScatter;
            Ray.TMin = 1e-4f;
            // Add a bounce.
            BounceIndex++;
        }
        // Case B: Hit Surface of Static Mesh
        else if (Payload.TCurrent < T_VolumeScatter && Payload.bIsSurfaceHit)
        {
            uint InstanceIndex = Payload.HitInstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
            IntersectionMaterial Intersection = EvaluateStaticMeshRenderableIntersectionMaterial(
                InstanceIndex,
                Payload.HitGeometryIndex,
                Payload.HitPrimitiveIndex,
                Payload.HitBarycentrics,
                0
            );


            // Simple alpha test
            if (rng.rand() > Intersection.Opacity)
            {
                // Pass static mesh: keep direction and move forward a little.
                Ray.TMin = Payload.TCurrent + 1e-6f;
            }
            else
            {
                // A new bounce
                // 1. Add emission radiance
                if (Payload.bIsFrontFace)
                {
                    Radiance += Intersection.Emission * Throughput;
                }
                // 2. Sample outgoing ray direction (if pass alpha test)
                ShadingMaterial M = GetShadingMaterial(Intersection);
                // if(dot(M.Normal, Ray.Direction) > 0) M.Normal = -M.Normal;
                float3 SampledDirection;
                // Sample BSDF
                float BsdfPdf = SampleBDSF(M, -Ray.Direction, rng.rand2(), SampledDirection);

                // Update throughput
                float3 BsdfVal = EvaluateBSDF(M, -Ray.Direction, SampledDirection);
                float CosTerm = abs(dot(M.Normal, SampledDirection));

                if (BsdfPdf > 1e-6f)
                {
                    Throughput *= BsdfVal * CosTerm / max(BsdfPdf, 1e-5f);
                }
                else
                {
                    break; // Absorbed or Invalid sample.
                }

                // Update ray origin
                Ray.Origin = Intersection.WorldPosition + Intersection.GeometryNormal * 2e-5f;
                Ray.Direction = SampledDirection;
                Ray.TMin = 1e-6f;
                BounceIndex++;
            }
        }
        // Case C: Volume Boundary Crossing (bIsSurfaceHit == false, Primitive or Grid)
        else if (Payload.TCurrent < T_VolumeScatter && !Payload.bIsSurfaceHit)
        {
            // Now we hit a proxy box. Update processing volume list and forward.
            uint InstanceIndex = Payload.HitInstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;

            // Determinate if it is a Primitive or a Grid based on Payload Flag
            if (Payload.bIsVolumeGridHit)
            {
                // --- Processing Volume Grid ---
                VolumeGridInstanceHeader Renderable = GetVolumeGridInstanceHeader(RenderableHeaderBuffer[InstanceIndex]);
                uint GridIndex = Renderable.VolumeGridIndex;

                if (!Payload.bIsFrontFace)
                {
                    // Back hit: ray is entering the volume grid.
                    if (CurrentOverlappingVolumeGridCount < MAX_OVERLAPPING_VOLUME_GRIDS)
                    {
                        OverlappingVolumeGridIndices[CurrentOverlappingVolumeGridCount] = GridIndex;
                        OverlappingVolumeGridsInstanceIndices[CurrentOverlappingVolumeGridCount] = InstanceIndex;
                        CurrentOverlappingVolumeGridCount++;
                    }
                }
                else
                {
                    // Front hit: ray is leaving the volume.
                    RemoveVolumeGrid(
                        GridIndex,
                        InstanceIndex,
                        CurrentOverlappingVolumeGridCount,
                        OverlappingVolumeGridIndices,
                        OverlappingVolumeGridsInstanceIndices
                    );
                }
            }
            else
            {
                // --- Processing Volume Primitives ---
                uint InstanceVolumePrimitiveIndex = Payload.HitPrimitiveIndex / 20;
                VolumePrimitivesInstanceHeader Renderable = GetVolumePrimitivesInstanceHeader(RenderableHeaderBuffer[InstanceIndex]);
                uint VolumePrimitiveOffset = VolumePrimitivesHeaderBuffer[Renderable.VolumePrimitivesIndex].PrimitiveOffset;
                uint PrimitiveIndex = VolumePrimitiveOffset + InstanceVolumePrimitiveIndex;

                if (!Payload.bIsFrontFace)
                {
                    // Back hit: ray is entering the volume.
                    if (CurrentOverlappingVolumePrimitiveCount < MAX_OVERLAPPING_VOLUME_PRIMITIVES)
                    {
                        OverlappingVolumePrimitiveIndices[CurrentOverlappingVolumePrimitiveCount] = PrimitiveIndex;
                        OverlappingVolumePrimitivesInstanceIndices[CurrentOverlappingVolumePrimitiveCount] = InstanceIndex;
                        CurrentOverlappingVolumePrimitiveCount++;
                    }
                }
                else
                {
                    // Front hit: ray is leaving the volume.
                    RemoveVolumePrimitive(
                        PrimitiveIndex,
                        InstanceIndex,
                        CurrentOverlappingVolumePrimitiveCount,
                        OverlappingVolumePrimitiveIndices,
                        OverlappingVolumePrimitivesInstanceIndices
                    );
                }
            }

            // Forward a bit.
            Ray.TMin = Payload.TCurrent + 1e-6f;
        }
        // Case D: Miss (Environment)
        else
        {
            float3 EnvironmentColor = EvaluateEnvironmentMap_Raw(-Ray.Direction, UB.EnvironmentMapLOD, UB.EnvironmentMapMultiplier);
            Radiance += Throughput * EnvironmentColor;
            // Terminate directly
            break;
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
    Payload.bIsVolumeGridHit = (InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_GRID) != 0;
}