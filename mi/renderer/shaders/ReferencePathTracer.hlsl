#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedDirectionalLight.hlsl"
#include "shared/SharedLight.hlsl"
#include "shared/SharedVolumeGrid.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/VolumeGridLib.hlsl"
#include "headers/Scattering.hlsl"
#include "headers/MaterialEvaluation.hlsl"
#include "headers/RayTracingHelpers.hlsl"

#include "resources/BindlessTextureResources.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"
#include "resources/EnvironmentLightResource.hlsl"
#include "resources/DirectionalLightResource.hlsl"
#include "shared/SharedLightClusterHierarchy.hlsl"

// LCH buffers for NEE light evaluation
StructuredBuffer<MeshLightInstanceTriangle> LCH_MeshLightInstanceTriangleBuffer;
StructuredBuffer<MeshLightInstance> LCH_MeshLightInstanceBuffer;
StructuredBuffer<MeshLight> LCH_MeshLightBuffer;

StructuredBuffer<VolumeGridHeader> VolumeGridHeaderBuffer;

// Top level AS
RaytracingAccelerationStructure PTLAS;

struct ReferencePathTracerUB {
    uint FrameIndex;
    uint EnableAccumulation;
    uint MaxNumBounces;
    float EnvironmentMapLOD;
    float3 EnvironmentMapMultiplier;
    uint NumMeshLightInstances; // total mesh light instances
    uint3 Padding;
};

ConstantBuffer<ReferencePathTracerUB> UB;


struct [raypayload] RayPayload {
    uint Mode; // 0: regular path ray, 1: directional shadow ray
    uint ShadowVisible;
    uint bIsSurfaceHit; // True if hit a surface, false if miss or hit a volume
    uint bIsVolumeGridHit; // True if hit a volume grid boundary
    uint bIsFrontFace;
    float TCurrent;
    uint HitInstanceCustomIndex;
    uint HitGeometryIndex;
    uint HitPrimitiveIndex;
    float2 HitBarycentrics;
};

[[vk::image_format("rgba32f")]]
RWTexture2D<float4> RWRadiance; // Output radiance (1spp)

// DLSS Ray Reconstruction auxiliary outputs (enabled when ENABLE_DLSS_RR is defined)
#ifdef ENABLE_DLSS_RR
[[vk::image_format("r32f")]]
RWTexture2D<float> RWDepth;
[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWNormal;
[[vk::image_format("rg16f")]]
RWTexture2D<float2> RWMotionVector;
[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWAlbedo;
[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWSpecularAlbedo;
[[vk::image_format("r8")]]
RWTexture2D<float> RWRoughness;
[[vk::image_format("r8")]]
RWTexture2D<float> RWAlpha;
#endif

#define MAX_OVERLAPPING_VOLUME_PRIMITIVES 16
#define MAX_OVERLAPPING_VOLUME_GRIDS 16

bool TraceDirectionalLightVisibility(float3 Origin, float3 GeometryNormal, float3 ToLightDirection,
    float MaxDistance = Infinity)
{
    RayDesc ShadowRay = (RayDesc)0;
    ShadowRay.Origin = OffsetRayOrigin(Origin, dot(GeometryNormal, ToLightDirection) >= 0.0f ? GeometryNormal : -GeometryNormal);
    ShadowRay.Direction = ToLightDirection;
    ShadowRay.TMin = 0.0f;
    ShadowRay.TMax = MaxDistance;

    RayPayload Payload = (RayPayload)0;
    Payload.Mode = 1;
    Payload.ShadowVisible = 1;
    Payload.TCurrent = ShadowRay.TMax;
    TraceRay(
        PTLAS,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xFF,
        0,
        0, // SBT stride (per-instance offset only)
        0,
        ShadowRay,
        Payload
    );
    return Payload.ShadowVisible != 0;
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

// ============================================================================
// NEE (Next Event Estimation) - Per-MLI Uniform Mesh Light Sampling
// ============================================================================

// NEE: Unified direct lighting sampling over directional light + mesh light instances.
// When bIsSurface=true, uses BSDF evaluation; when false (volume scatter), uses phase function.
// Albedo is the volume scattering color (only used when bIsSurface=false).
// Returns the direct lighting contribution (to be multiplied by Throughput externally).
// TODO implement real LCH importance sampling
float3 NEESampleDirectLighting(
    float3 Position, float3 ShadingNormal, float3 ViewDirection,
    ShadingMaterial Mat, bool bIsSurface, float3 Albedo, float HG_g,
    inout Random rng
)
{

    float3 DirectLighting = 0;
    uint NumMLIs = UB.NumMeshLightInstances;
    bool bHasDirLight = DirectionalLightEnabled();

    if (NumMLIs == 0 && !bHasDirLight) return 0;

    // Selection probabilities: equal-probability between available light types
    float DirProb = bHasDirLight ? 0.5f : 0.0f;
    float AreaProb = (NumMLIs > 0) ? (1.0f - DirProb) : 0.0f;

    if (rng.rand() < DirProb)
    {
        // --- Directional Light ---
        float3 LightDirection = GetDirectionalLightDirection();
        float NoL = dot(ShadingNormal, LightDirection);
        if (NoL <= 0.0f) return 0;

        if (!TraceDirectionalLightVisibility(Position, ShadingNormal, LightDirection)) return 0;

        float3 Irradiance = GetDirectionalLightIrradiance();
        float Pdf = DirProb; // delta light: solid angle PDF = 1

        if (bIsSurface)
        {
            float3 BSDF = EvaluateBSDF(Mat, ViewDirection, LightDirection);
            DirectLighting = BSDF * Irradiance * NoL / Pdf;
        }
        else
        {
            float Phase = HenyeyGreensteinPhaseFunction(dot(LightDirection, ViewDirection), HG_g);
            DirectLighting = Albedo * Irradiance * Phase / Pdf;
        }
    }
    else
    {
        // --- Area Light (per-MLI uniform sampling) ---
        if (NumMLIs == 0) return 0;

        // 1. Select a random MeshLightInstance
        uint MLIIndex = min(uint(rng.rand() * NumMLIs), NumMLIs - 1u);
        MeshLightInstance MLI = LCH_MeshLightInstanceBuffer[MLIIndex];
        MeshLight ML = LCH_MeshLightBuffer[MLI.MeshLightIndex];

        if (ML.NumTriangles == 0) return 0;

        // 2. Read emission from material (same material for all triangles of this mesh light).
        //    MLITri.Intensity is only used for importance sampling, not shading.
        StaticMeshHeader MeshHeader = StaticMeshHeaderBuffer[ML.StaticMeshIndex];
        uint DescriptionIndex = MeshHeader.DescriptionOffset + ML.StaticMeshDescriptionIndex;
        uint MaterialIndex = StaticMeshDescriptionBuffer[DescriptionIndex].y;
        MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
        float3 Emission = Material.Emissive;
        if (IsValid(Material.EmissiveMap))
            Emission += GetBindlessSRV(Material.EmissiveMap).SampleLevel(LinearWrapSampler, float2(0, 0), 0).rgb;
        if (dot(Emission, 1.f.xxx) <= 0.0f) return 0;

        // 3. Select a random triangle within this MLI
        uint LocalTriIndex = min(uint(rng.rand() * ML.NumTriangles), ML.NumTriangles - 1u);
        uint TriangleIndex = MLI.MeshLightInstanceTriangleOffset + LocalTriIndex;
        MeshLightInstanceTriangle MLITri = LCH_MeshLightInstanceTriangleBuffer[TriangleIndex];

        float3 Edge0 = MLITri.V1 - MLITri.V0;
        float3 Edge1 = MLITri.V2 - MLITri.V0;
        float TriangleArea = length(cross(Edge0, Edge1)) * 0.5f;
        if (TriangleArea < 1e-8f) return 0;

        // 4. Sample a point on the triangle using uniform barycentric coordinates
        float2 u = rng.rand2();
        if (u.x + u.y > 1.0f) u = 1.0f - u;
        float3 LightPoint = MLITri.V0 + u.x * Edge0 + u.y * Edge1;

        float3 ToLight = LightPoint - Position;
        float DistanceSq = dot(ToLight, ToLight);
        float Distance = sqrt(DistanceSq);
        float3 LightDirection = ToLight / max(Distance, 1e-6f);

        // Receiver normal check
        float NoL = dot(ShadingNormal, LightDirection);
        if (NoL <= 0.0f) return 0;

        // Light normal check
        float3 LightNormal = normalize(cross(Edge0, Edge1));
        float LightNoL = dot(LightNormal, -LightDirection);
        if (LightNoL <= 0.0f) return 0;

        // Shadow ray — offset both endpoints to avoid self-intersection (RTG Ch.6)
        float3 OffsetLightPoint = OffsetRayOrigin(LightPoint, LightNormal);
        float3 OffsetReceiver = OffsetRayOrigin(Position, dot(ShadingNormal, LightDirection) >= 0.0f ? ShadingNormal : -ShadingNormal);
        float3 ToOffsetLight = OffsetLightPoint - OffsetReceiver;
        float ShadowTMax = length(ToOffsetLight);
        float3 ShadowDir = ToOffsetLight / max(ShadowTMax, 1e-7f);
        bool bShadowVisible = TraceDirectionalLightVisibility(OffsetReceiver, ShadingNormal, ShadowDir, ShadowTMax);
        if (!bShadowVisible) return 0;

        // PDF: P(select MLI) * P(select triangle within MLI) * area-to-solid-angle
        float SelectionPdf = (AreaProb / float(NumMLIs)) * (1.0f / float(ML.NumTriangles));
        float SolidAnglePdf = DistanceSq / max(abs(LightNoL) * TriangleArea, 1e-6f);
        float TotalPdf = SelectionPdf * SolidAnglePdf;

        if (TotalPdf > 1e-8f)
        {
            if (bIsSurface)
            {
                float3 BSDF = EvaluateBSDF(Mat, ViewDirection, LightDirection);
                DirectLighting = BSDF * Emission * NoL * LightNoL / TotalPdf;
            }
            else
            {
                float Phase = HenyeyGreensteinPhaseFunction(dot(LightDirection, ViewDirection), HG_g);
                DirectLighting = Albedo * Emission * Phase * LightNoL / TotalPdf;
            }
        }
    }
    return DirectLighting;
}

#include "resources/GigaVoxelResources.hlsl"
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

#ifdef ENABLE_DLSS_RR
    // Track primary ray hit info for DLSS auxiliary buffers
    float  PrimaryRayT = 0;
    float3 PrimaryRayDir = Ray.Direction; // Save before bounce loop modifies Ray.Direction
    float3 PrimaryHitNormal = 0;
    float3 PrimaryHitAlbedo = 0;
    float3 PrimaryHitSpecularAlbedo = 0;
    float  PrimaryHitRoughness = 0;
    bool   PrimaryHitSurface = false;
#endif
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

#ifndef ENABLE_DLSS_RR
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
#endif

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
        PTLAS,
            0,
            0xFF, // Ray mask
            0, // Suface ray
            0, // SBT stride (per-instance offset only)
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
            float3 ScatterPosition = Ray.Origin + Ray.Direction * T_VolumeScatter;

            // NEE: Direct lighting from directional light + area lights
            {
                float3 NEERadiance = NEESampleDirectLighting(
                    ScatterPosition, float3(0,0,0), -Ray.Direction,
                    (ShadingMaterial)0, false, VolumeSampledColor, g, rng
                );
                Radiance += Throughput * NEERadiance;
            }

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
            Ray.Origin = ScatterPosition;
            Ray.TMin = 1e-4f;
            // Add a bounce.
            BounceIndex++;
        }
        // Case B: Hit Surface of Static Mesh
        else if (Payload.TCurrent < T_VolumeScatter && Payload.bIsSurfaceHit)
        {
            uint InstanceIndex = Payload.HitInstanceCustomIndex;  // StaticMesh: InstanceID() == RenderableIndex
            IntersectionMaterial Intersection = EvaluateStaticMeshRenderableIntersectionMaterial(
                InstanceIndex,
                Payload.HitGeometryIndex,
                Payload.HitPrimitiveIndex,
                Payload.HitBarycentrics,
                0
            );

#ifdef ENABLE_DLSS_RR
            if (BounceIndex == 0) {
                PrimaryHitSurface = true;
                PrimaryRayT = Payload.TCurrent;
                ShadingMaterial M0 = GetShadingMaterial(Intersection);
                float NormalFlipping0 = dot(Intersection.GeometryNormal, Ray.Direction) > 0 ? -1 : 1;
                PrimaryHitNormal = NormalFlipping0 * Intersection.GeometryNormal;
                if(dot(Intersection.GeometryNormal, Ray.Direction) > 0) {
                    M0.Normal = -M0.Normal;
                }
                PrimaryHitAlbedo = M0.Albedo;
                PrimaryHitSpecularAlbedo = lerp(float3(0.04, 0.04, 0.04), M0.Albedo, M0.Metallic);
                PrimaryHitRoughness = M0.Roughness;
            }
#endif


            // Simple alpha test
            if (rng.rand() > Intersection.Opacity)
            {
                // Pass static mesh: keep direction and move forward a little.
                Ray.TMin = Payload.TCurrent + 1e-6f;
            }
            else
            {
                // A new bounce
                // 1. Add emission radiance (only on camera ray to avoid double-counting with NEE)
                if (BounceIndex == 0 && Payload.bIsFrontFace)
                {
                    Radiance += Intersection.Emission * Throughput;
                }
                // 2. Sample outgoing ray direction (if pass alpha test)
                ShadingMaterial M = GetShadingMaterial(Intersection);
                float NormalFlipping = dot(Intersection.GeometryNormal, Ray.Direction) > 0 ? -1 : 1;
                if(dot(Intersection.GeometryNormal, Ray.Direction) > 0) {
                    // Invert the shading normal if the ray hit the back face, to make it consistent with the single-sided shading model.
                    M.Normal = -M.Normal;
                }

                // 3. NEE: Direct lighting from directional light + area lights
                {
                    float3 NEERadiance = NEESampleDirectLighting(
                        Intersection.WorldPosition, M.Normal, -Ray.Direction, M, true, float3(0,0,0), 0.f, rng
                    );
                    Radiance += Throughput * NEERadiance;
                }

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
                Ray.Origin = Intersection.WorldPosition + NormalFlipping * Intersection.GeometryNormal * 2e-5f;
                Ray.Direction = SampledDirection;
                Ray.TMin = 1e-6f;
                BounceIndex++;
            }
        }
        // Case C: Volume Boundary Crossing (bIsSurfaceHit == false, Primitive or Grid)
        else if (Payload.TCurrent < T_VolumeScatter && !Payload.bIsSurfaceHit)
        {
#ifndef ENABLE_DLSS_RR
            // Now we hit a proxy box. Update processing volume list and forward.
            uint InstanceIndex = Payload.HitInstanceCustomIndex;  // VolumeGrid: InstanceID() == RenderableIndex

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
#endif
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

#ifdef ENABLE_DLSS_RR
    // Compute linear depth from primary ray hit (also used for DLSS depth output).
    // IMPORTANT: Use PrimaryRayDir (saved before bounce loop), not Ray.Direction (modified by bounces).
    float LinearDepth = PrimaryHitSurface
        ? PrimaryRayT * dot(PrimaryRayDir, C.Direction)
        : 0;

    // Compute motion vector from primary hit world position reprojected to previous frame NDC.
    // If no surface hit was made (miss or volume), we fall back to far-plane reprojection.
    float2 MotionVector = 0;
    {
        CameraParameters PrevC = GetPreviousCamera();
        float2 UV = ((float2)RayIndex + 0.5f.xx) / (float2)DispatchSize;
        float2 NDC2 = UVToNDC2(UV);
        // Use actual perspective Z depth for reprojection. The Reprojection matrix is built with
        // normal-Z (perspectiveRH_ZO), so we convert linear depth to perspective Z [0=near, 1=far].
        // For miss pixels (LinearDepth=0), use Z=1 (far plane) to avoid division by zero
        // in LinearDepthToZDepth and get a stable far-plane reprojection.
        float NDC_Z = PrimaryHitSurface ? LinearDepthToZDepth(C, LinearDepth) : 1.0f;
        float3 Reprojected = ReprojectToPreviousNDCFromNDC(C, float3(NDC2, NDC_Z));
        float2 PrevNDC2 = Reprojected.xy;
        // Remove jitter from both frames to get pure camera/object motion
        MotionVector = (NDC2 - C.Jitter) - (PrevNDC2 - PrevC.PrevJitter);
    }
    RWMotionVector[RayIndex] = MotionVector;

    // Write depth: linear depth from primary ray hit distance
    RWDepth[RayIndex] = LinearDepth;

    // Write normal: world-space shading normal packed to [0,1]
    RWNormal[RayIndex] = float4(PrimaryHitNormal * 0.5 + 0.5, 1);

    // Write albedo
    RWAlbedo[RayIndex] = float4(PrimaryHitAlbedo, 1);

    // Write specular albedo (F0)
    RWSpecularAlbedo[RayIndex] = float4(PrimaryHitSpecularAlbedo, 1);

    // Write roughness
    RWRoughness[RayIndex] = PrimaryHitRoughness;

    // Write alpha: 1 if primary ray hit a surface, 0 if miss/volume
    RWAlpha[RayIndex] = PrimaryHitSurface ? 1.0f : 0.0f;
#endif
}

[shader("miss")]
void ReferencePathTracerMiss(inout RayPayload Payload: SV_RayPayload) {
    if (Payload.Mode == 1) {
        Payload.ShadowVisible = 1;
        return;
    }
    Payload.HitInstanceCustomIndex = 0xFFFFFFFF;
}

// ============================================================================
// AnyHit shaders: only used for shadow rays (Payload.Mode == 1)
// ============================================================================

// StaticMesh anyhit: alpha test for shadow rays
[shader("anyhit")]
void ReferencePathTracerAnyHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    if (Payload.Mode != 1) {
        return;
    }

    uint Triangle            = PrimitiveIndex();
    uint DescriptionIndex    = GeometryIndex();
    uint Instance            = InstanceID();  // StaticMesh: InstanceID() == RenderableIndex

    StaticMeshInstanceHeader InstanceHeader = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[Instance]);
    uint StaticMeshIndex = InstanceHeader.StaticMeshIndex;
    uint DescriptionOffset = StaticMeshHeaderBuffer[StaticMeshIndex].DescriptionOffset;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[DescriptionOffset + DescriptionIndex];
    uint GeometryIndex = GeometryMaterialPair.x;
    uint MaterialIndex = GeometryMaterialPair.y;
    GeometryHeader Geometry = GeometryHeaderBuffer[GeometryIndex];
    uint IndexOffset = Geometry.IndexOffset + Triangle * 3;
    uint VertexOffset = Geometry.VertexOffset;

    uint VertexAIndex = VertexOffset + IndexBuffer[IndexOffset + 0];
    uint VertexBIndex = VertexOffset + IndexBuffer[IndexOffset + 1];
    uint VertexCIndex = VertexOffset + IndexBuffer[IndexOffset + 2];
    DefaultStaticMeshVertex VertexA = VertexBuffer[VertexAIndex];
    DefaultStaticMeshVertex VertexB = VertexBuffer[VertexBIndex];
    DefaultStaticMeshVertex VertexC = VertexBuffer[VertexCIndex];
    DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

    MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
    float4 ColorOpacity = float4(Material.Albedo, 1.0f);
    if (Material.AlbedoMap != INVALID_UINT) {
        ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
    }
    if (ColorOpacity.a < 0.1f) {
        IgnoreHit();
    }
}



// VolumeGrid anyhit: shadow rays should not hit volume grids
[shader("anyhit")]
void ReferencePathTracerAnyHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    if (Payload.Mode != 1) {
        return;
    }
    IgnoreHit();
}

// ============================================================================
// ClosestHit shaders: record hit info for path tracing
// ============================================================================

// Records the common hit data (geometry/primitive/bary/front-face/instance id)
// shared by every closesthit variant. The renderable-type-specific flags
// (bIsSurfaceHit / bIsVolumeGridHit) are set by the per-type wrappers below --
// renderable type is now determined by the SBT hit group, not by any bits in
// InstanceCustomIndex (the old class-bit decode was incompatible with
// GigaVoxel's [RTHeaderIndex:8][chunk_header_index:16] layout).
void ReferencePathTracerRecordClosestHitCommon(inout RayPayload Payload: SV_RayPayload,
                                               BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    if (Payload.Mode == 1) {
        Payload.ShadowVisible = 0;
        Payload.TCurrent = RayTCurrent();
        return;
    }
    Payload.TCurrent = RayTCurrent();
    Payload.HitInstanceCustomIndex = InstanceID();
    Payload.HitGeometryIndex = GeometryIndex();
    Payload.HitPrimitiveIndex = PrimitiveIndex();
    Payload.HitBarycentrics = Attributes.barycentrics;
    Payload.bIsFrontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE ? 1 : 0;
}

[shader("closesthit")]
void ReferencePathTracerClosestHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    ReferencePathTracerRecordClosestHitCommon(Payload, Attributes);
    Payload.bIsSurfaceHit = 1;
    Payload.bIsVolumeGridHit = 0;
}



[shader("closesthit")]
void ReferencePathTracerClosestHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    ReferencePathTracerRecordClosestHitCommon(Payload, Attributes);
    Payload.bIsSurfaceHit = 0;
    Payload.bIsVolumeGridHit = 1;
}

// GigaVoxel: VC chunk geometry. Shadow rays ignore it; path-trace rays record hit.
[shader("anyhit")]
void ReferencePathTracerAnyHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                               BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    if (Payload.Mode != 1) {
        return;
    }
    IgnoreHit();
}
[shader("closesthit")]
void ReferencePathTracerClosestHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    ReferencePathTracerRecordClosestHitCommon(Payload, Attributes);
    // GigaVoxel hits currently have no dedicated shading branch in the main loop;
    // flag neither surface nor volume grid so the generic miss/no-hit path applies.
    Payload.bIsSurfaceHit = 0;
    Payload.bIsVolumeGridHit = 0;
}
