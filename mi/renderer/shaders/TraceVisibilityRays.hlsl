#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedGaussianRadianceField.hlsl"
#include "shared/SharedVertex.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "headers/Random.hlsl"
#include "headers/Material.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/GaussianRadianceFieldResources.hlsl"

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

RaytracingAccelerationStructure TLAS;

StructuredBuffer<RenderableHeader> RenderableHeaderBuffer;
StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;
StructuredBuffer<PackedVolumePrimitive> PrimitiveData;

Texture2D<float> G_Depth;

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

[shader("raygeneration")]
void TraceVisibilityRaysRaygen() {
#ifdef USE_RAY_LIST
    uint RayListIndex = DispatchRaysIndex().x;
    uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
    uint RayIndex = DispatchRaysIndex().x;
#endif

    RayDesc Ray = (RayDesc)0;
    {
        CameraParameters C = GetActiveCamera();
#ifndef USE_SCREEN_COORDS
        Ray.Origin = RayToTraceOriginBuffer[RayIndex];
#else 
        uint2 PixelIndex = UnpackUint2x16(RayToTraceOriginScreenCoordBuffer[RayIndex]);
        float2 UV = (PixelIndex + 0.5f) * C.InvFilmDimensions;
        float ReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, UV, 0);
        float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
        Ray.Origin = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
#endif
        Ray.Direction = RayToTraceDirectionBuffer[RayIndex];
        bool bHit = false;
        Ray.TMin = UnpackRayToTraceState(RWRayToTraceStateBuffer[RayIndex], bHit);
#ifdef USE_RAY_TMAX_BUFFER
        Ray.TMax = RayToTraceTMaxBuffer[RayIndex];
#else
        Ray.TMax = C.FarPlane;
#endif
    }

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
        TLAS,
        // Proxy geometries of the volume primitives are built face-flipped. So culling back faces
        // means culling real front faces for volume primitives. Only real back faces from volume
        // primitives are hit when tracing the ray.
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF, // Ray mask
        0,    // SBT offset
        0,    // SBT stride
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
    Payload.PackedMaterial = MakePackedInvalidCachedHitMaterial();
}


RayDesc GetRayDesc () {
    RayDesc Ray;
    Ray.Origin = WorldRayOrigin();
    Ray.Direction = WorldRayDirection();
    Ray.TMin = RayTMin();
    Ray.TMax = RayTCurrent();
    return Ray;
}

[shader("anyhit")]
void TraceVisibilityRaysAnyHit(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_NONE) {
        // Static mesh instance
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

        // Interpolate the vertex
        DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

        MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
        float4 ColorOpacity = float4(Material.Albedo, 1);
        if(IsValid(Material.AlbedoMap)) {
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
        }

	    if(ColorOpacity.a < Payload.U) {
            Payload.U = (Payload.U - ColorOpacity.a) / (1.f - ColorOpacity.a);
            // Semi-transparent surfaces, continue tracing
 		    IgnoreHit();
	    } else {
            // There'll be systematically more 'transparent' volumes near surfaces
            // with this tracing method. We just let that happen.
            Payload.U = Payload.U / max(ColorOpacity.a, 1e-5f);
        }
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES) {
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the volume primitive (each volume primitive have 20 triangles for proxy geometry) 
        uint InstancePrimitiveIndex = PrimitiveIndex() / 20;
        uint PrimitiveOffset = VolumePrimitivesHeaderBuffer[Instance].PrimitiveOffset;
        uint PrimitiveIndex = PrimitiveOffset + InstancePrimitiveIndex;
        VolumePrimitive Primitive = UnpackVolumePrimitive(PrimitiveData[PrimitiveIndex]);
        float3x4 ToObject = WorldToObject3x4();
        float2 lr = 0;
        float Dist = 0;
        bool bIntersected = RayIntersect(RayOrigin, RayDirection, Primitive, ToObject, lr, Dist);
        if(bIntersected) {
            float TMin = RayTMin();
            lr.x = max(lr.x, TMin);
            lr.y = max(lr.y, TMin);
            float Length = max(lr.y - lr.x, 0);
            float Opacity = Primitive.Opacity * VolumePrimitiveRayDecay(Dist);
            
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
// Full visibility
            #error "not implemented yet"
#elif VISIBILITY_TRACE_TYPE==VISIBILITY_TRACE_TYPE_COARSE_WITH_EXACT_VOLUME_SCATTERING
// Coarse visibility with precise intersection sampling
            float FlyDist = SampleExponentialScatteringMedium(Opacity, Payload.U);
            float Transmittance = IntegrateExponentialScatteringMedium(Opacity, Length);
            if(FlyDist < Length) {
                // The ray spawned a scattering event in the volume
                Payload.U = saturate(Payload.U / max(1e-6f, 1 - Transmittance));
                if(Payload.HitDistance > lr.x + FlyDist) {
                    // Closer than previous hit, update the hit info
                    Payload.HitDistance = lr.x + FlyDist;
                    Payload.PackedMaterial = PackCachedHitMaterial(MakeCachedHitMaterial(Primitive.Color, CACHED_HIT_MATERIAL_HIT_TYPE_VOLUME)); 
                }
            } else {
                // The ray passed through the volume
                Payload.U = saturate(Payload.U / max(1e-6f, Transmittance));
            }
            bool bShouldIgnoreHit = true;
            // Report a hit event if the closer-volume boundary is further than the current hit distance
            // with an adaptive bias. Thus we can cull volume primitives that are not likely to generate
            // a scattering event before the current hit distance.
            float Bias = UB.VolumeScatteringEventShellHitCullingBias;
            if(Payload.HitDistance + Bias < lr.x) {
                bShouldIgnoreHit = false;
            }
            if(bShouldIgnoreHit) {
                IgnoreHit();
            }
#else
// Coarse visibility
            float Transmittance = exp(-Length * Opacity);
            if(Transmittance < Payload.U) {
                // The ray is absorbed in the volume
                // ...assume the hit is on the primitive's proxy backface
                Payload.U = (Payload.U - Transmittance) / (1.f - Transmittance);
            } else {
                Payload.U = Payload.U / max(Transmittance, 1e-5f);
                IgnoreHit();
            }
#endif
        }
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_GAUSSIAN_RADIANCE_FIELD) {
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
// Full visibility
            #error "not implemented yet"
#else
        // 3D gaussian radiance field instance
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the 3d gaussian (each 3d gaussian have 20 triangles for proxy geometry) 
        uint InstanceGaussianIndex = PrimitiveIndex() / 20;
        uint GaussianOffset = GaussianRadianceFieldHeaderBuffer[Instance].PointOffset;
        uint GaussianIndex = GaussianOffset + InstanceGaussianIndex;
        Gaussian3D G = UnpackGaussian(Gaussian3DBuffer[GaussianIndex]);
        RayDesc Ray = GetRayDesc();
        float RayScaler = 1, RayT = 0;
        float3x4 WorldToObject = WorldToObject3x4();
        float3x3 WorldToObjectNormal = RenderableNormalTransformBuffer[Instance];
        float3 LocalRayOrigin = TransformPoint(WorldToObject, Ray.Origin);
        float3 RayTangent, RayBitangent;
        GetOrthoVectors(Ray.Direction, RayTangent, RayBitangent);
        float3 LocalRayDirection = TransformVector(WorldToObject, Ray.Direction);
        float3 LocalRayTangent   = TransformVector(WorldToObjectNormal, RayTangent);
        float3 LocalRayBitangent = TransformVector(WorldToObjectNormal, RayBitangent);
        float3x3 RaySpace = float3x3(
            LocalRayTangent / dot(LocalRayTangent, LocalRayTangent),
            LocalRayBitangent / dot(LocalRayBitangent, LocalRayBitangent),
            LocalRayDirection / dot(LocalRayDirection, LocalRayDirection)
        );
        float Alpha = 
            EvaluateGaussianResponseRast(LocalRayOrigin, RaySpace, G, RayT);
        // Stochastically ignore the hit according to the gaussian response (opacity along the ray)
        bool bShouldIgnoreHit = Alpha < Payload.U;
        if(bShouldIgnoreHit) {
            Payload.U = (Payload.U - Alpha) / (1 - Alpha);
        } else { // Hit
            // Still, we have to roll out a new seed for the next possible anyhit
            Payload.U = Payload.U / Alpha;
            Payload.HitDist = RayT / max(1e-6, RayScaler); // Use the max response T as reply
        }
#endif
    } else {
        // Unknown instance type, just continue tracing
        IgnoreHit();
    }
}

[shader("closesthit")]
void TraceVisibilityRaysClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    Payload.HitDistance = RayTCurrent();
#if VISIBILITY_TRACE_TYPE == VISIBILITY_TRACE_TYPE_FULL
// Full visibility
#error "not implemented yet"
#elif VISIBILITY_TRACE_TYPE==VISIBILITY_TRACE_TYPE_COARSE_WITH_EXACT_VOLUME_SCATTERING
// Coarse output, but precise intersection sampling is applied in volume primitives
    if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_NONE) {
        // Mesh surface hit
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

        // Interpolate the vertex
        DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

        MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
        float4 ColorOpacity = float4(Material.Albedo, 1);
        if(IsValid(Material.AlbedoMap)) {
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
        }
        float3 Normal = InterpolatedVertex.Normal;
        float3x3 NormalTransform = transpose(To3x3(WorldToObject3x4()));
        Normal = normalize(mul(NormalTransform, Normal));
        CachedHitMaterial CachedHitMat = MakeCachedHitMaterial(ColorOpacity.rgb, CACHED_HIT_MATERIAL_HIT_TYPE_SURFACE, Normal);
        Payload.PackedMaterial = PackCachedHitMaterial(CachedHitMat);
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES) {
        // Leaving the data coming from the any-hit shader unchanged is ok. 
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_GAUSSIAN_RADIANCE_FIELD) {
        // 3D gaussian radiance field instance
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the 3d gaussian (each 3d gaussian have 20 triangles for proxy geometry) 
        uint InstanceGaussianIndex = PrimitiveIndex() / 20;
        uint GaussianOffset = GaussianRadianceFieldHeaderBuffer[Instance].PointOffset;
        uint GaussianIndex = GaussianOffset + InstanceGaussianIndex;
        sadfasdfdasfasdfasdfas (evaluate gaussian)
        Payload.y = PackCachedHitMaterial(MakeCachedHitMaterialForGaussianRF(Instance));
    } else {
        // Unknown instance type, do nothing
    }
#else // VISIBILITY_TRACE_TYPE==VISIBILITY_TRACE_TYPE_COARSE
// Coarse visibility
    if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_NONE) {
        // Mesh surface hit
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

        // Interpolate the vertex
        DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

        MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
        float4 ColorOpacity = float4(Material.Albedo, 1);
        if(IsValid(Material.AlbedoMap)) {
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
        }
        float3 Normal = InterpolatedVertex.Normal;
        float3x3 NormalTransform = transpose(To3x3(WorldToObject3x4()));
        Normal = normalize(mul(NormalTransform, Normal));
        CachedHitMaterial CachedHitMat = MakeCachedHitMaterial(ColorOpacity.rgb, CACHED_HIT_MATERIAL_HIT_TYPE_SURFACE, Normal);
        Payload.PackedMaterial = PackCachedHitMaterial(CachedHitMat);
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES) {
        uint InstancePrimitiveIndex = PrimitiveIndex() / 20;
        uint PrimitiveOffset = VolumePrimitivesHeaderBuffer[Instance].PrimitiveOffset;
        uint PrimitiveIndex = PrimitiveOffset + InstancePrimitiveIndex;
        VolumePrimitive Primitive = UnpackVolumePrimitive(PrimitiveData[PrimitiveIndex]);

        CachedHitMaterial CachedHitMat = MakeCachedHitMaterial(Primitive.Color, CACHED_HIT_MATERIAL_HIT_TYPE_VOLUME);
        Payload.PackedMaterial = PackCachedHitMaterial(CachedHitMat);
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_GAUSSIAN_RADIANCE_FIELD) {
        // 3D gaussian radiance field instance
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the 3d gaussian (each 3d gaussian have 20 triangles for proxy geometry) 
        uint InstanceGaussianIndex = PrimitiveIndex() / 20;
        uint GaussianOffset = GaussianRadianceFieldHeaderBuffer[Instance].PointOffset;
        uint GaussianIndex = GaussianOffset + InstanceGaussianIndex;
        sdfasfasdfasfasd (evaluate gaussian radiance)
        Payload.PackedMaterial = PackCachedHitMaterial(MakeCachedHitMaterialForGaussianRF(Instance));
    } else {
        // Unknown instance type, do nothing
    }
#endif
}