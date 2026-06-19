#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedVolumeGrid.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "headers/Random.hlsl"
#include "headers/RayTracingHelpers.hlsl"
#include "headers/GaussianSplatting.hlsl"
#include "headers/VolumeGridLib.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/RenderableResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/GaussianRadianceFieldResources.hlsl"

struct TraceTransmittanceRaysUB {
    uint Seed;
    uint3 Padding;
};
ConstantBuffer<TraceTransmittanceRaysUB> UB;

RaytracingAccelerationStructure PTLAS;

StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;
StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
StructuredBuffer<VolumeGridHeader> VolumeGridHeaderBuffer;


Texture2D<float> G_Depth;
Texture2D<uint> G_GeometryNormal;

StructuredBuffer<uint> RayToTraceListLengthBuffer;
StructuredBuffer<uint> RayToTraceListBuffer;

StructuredBuffer<float3> RayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;
RWStructuredBuffer<float> RWRayToTraceTransmittanceBuffer;

// Optional (when the starting point is exactly on a pixel center)
StructuredBuffer<uint> RayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
StructuredBuffer<float3> RayToTraceOriginBuffer;

// Optional (when the ray tmax is passed as a parameter, otherwise defaults to far plane)
StructuredBuffer<float> RayToTraceTMaxBuffer; 

#include "headers/HardwareRayTracing.hlsl"

struct [raypayload] RayPayload {
    float HitDistance; // Hit on meshes, TMax for no hits
    float Transmittance; // Transmittance value for the ray
};

#include "resources/GigaVoxelResources.hlsl"
[shader("raygeneration")]
void TraceTransmittanceRaysRaygen() {
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
    Payload.Transmittance = 1.0f; // Default transmittance value, will be updated in any hits
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
    uint PackedTraceResult = PackRayToTraceState(Payload.HitDistance, Payload.HitDistance < Ray.TMax);
    RWRayToTraceStateBuffer[RayIndex] = PackedTraceResult;
    // Write back transmittance
    RWRayToTraceTransmittanceBuffer[RayIndex] = Payload.Transmittance;
}

[shader("miss")]
void TraceTransmittanceRaysMiss(inout RayPayload Payload: SV_RayPayload) {
    // ...
}

// StaticMesh anyhit: alpha test, accumulate transmittance for semi-transparent surfaces
[shader("anyhit")]
void TraceTransmittanceRaysAnyHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID();
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;

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
    // Semi-transparent surfaces
    Payload.Transmittance *= saturate(1.f - ColorOpacity.a);
    if(ColorOpacity.a < 0.99f) {
        IgnoreHit();
    }
}

// VolumePrimitives anyhit: compute transmittance through volume primitive
[shader("anyhit")]
void TraceTransmittanceRaysAnyHit_VolumePrimitives(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    float3 RayOrigin = WorldRayOrigin();
    float3 RayDirection = WorldRayDirection();
    uint InstanceCustomIndex = InstanceID();
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    // Get the index of the volume primitive (each volume primitive have 20 triangles for proxy geometry) 
    uint InstancePrimitiveIndex = PrimitiveIndex() / 20;
    VolumePrimitivesInstanceHeader InstanceHeader = GetVolumePrimitivesInstanceHeader(RenderableHeaderBuffer[Instance]);
    uint VolprimsIndex = InstanceHeader.VolumePrimitivesIndex;
    uint PrimitiveOffset = VolumePrimitivesHeaderBuffer[VolprimsIndex].PrimitiveOffset;
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
        // Multiply to transmittance
        float Transmittance = exp(-Length * Opacity);
        Payload.Transmittance *= Transmittance;
        if(Payload.Transmittance < 0.001f) {
            // Early termination if transmittance is too small
            AcceptHitAndEndSearch();
        }
    }
    // Always ignore hits on volume primitives
    IgnoreHit();
}

// GaussianRadianceField anyhit: compute alpha response
[shader("anyhit")]
void TraceTransmittanceRaysAnyHit_GaussianRadianceField(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    float3 RayOrigin = WorldRayOrigin();
    float3 RayDirection = WorldRayDirection();
    uint InstanceCustomIndex = InstanceID();
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    // Get the index of the 3d gaussian (each 3d gaussian have 20 triangles for proxy geometry) 
    uint InstanceGaussianIndex = PrimitiveIndex() / 20;
    GaussianRadianceFieldInstanceHeader GRFInstanceHeader = GetGaussianRadianceFieldInstanceHeader(RenderableHeaderBuffer[Instance]);
    uint RadianceFieldIndex = GRFInstanceHeader.FieldIndex;
    uint GaussianOffset = GaussianRadianceFieldHeaderBuffer[RadianceFieldIndex].PointOffset;
    uint GaussianIndex = GaussianOffset + InstanceGaussianIndex;
    Gaussian3D G = UnpackGaussian(Gaussian3DBuffer[GaussianIndex]);
    RayDesc Ray = GetRayDesc();
    float RayScaler = 1, RayT = 0;
    float3x4 WorldToObject = WorldToObject3x4();
    float3x3 WorldToObjectNormal = transpose(To3x3(WorldToObject3x4()));
    float3 LocalRayOrigin = TransformPoint(WorldToObject, Ray.Origin);
    float3 RayTangent, RayBitangent;
    GetOrthoVectors(Ray.Direction, RayTangent, RayBitangent);
    float3 LocalRayDirection = TransformVector(WorldToObject, Ray.Direction);
    float3 LocalRayTangent   = TransformVector(WorldToObjectNormal, RayTangent);
    float3 LocalRayBitangent = TransformVector(WorldToObjectNormal, RayBitangent);
    // Ray space basis vectors are not normalized. Thus it can correctly capture the non-uniform scaling in
    // the transformation, which is important for correct evaluation of the Gaussian response.
    float3x3 RaySpace = float3x3(
        LocalRayTangent   / dot(LocalRayTangent, LocalRayTangent),
        LocalRayBitangent / dot(LocalRayBitangent, LocalRayBitangent),
        LocalRayDirection / dot(LocalRayDirection, LocalRayDirection)
    );
    float Alpha = 
        EvaluateGaussianResponseRast(LocalRayOrigin, RaySpace, G, RayT);
    Payload.Transmittance *= Alpha;
    if(Payload.Transmittance < 0.001f) {
        // Early termination if transmittance is too small
        AcceptHitAndEndSearch();
    }
    // Always ignore hits on gaussian RF
    IgnoreHit();
}

// VolumeGrid anyhit: ratio tracking through volume grid
[shader("anyhit")]
void TraceTransmittanceRaysAnyHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint InstanceCustomIndex = InstanceID();
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    VolumeGridInstanceHeader InstanceHeader = GetVolumeGridInstanceHeader(RenderableHeaderBuffer[Instance]);
    uint GridIndex = InstanceHeader.VolumeGridIndex;
    VolumeGridHeader Grid = VolumeGridHeaderBuffer[GridIndex];

    float3 RayOrigin = WorldRayOrigin();
    float3 RayDirection = WorldRayDirection();
    float TMin = RayTMin();
    float TMax = RayTCurrent();

    float3x4 WorldToObject = WorldToObject3x4();
    float3 localRayOrigin = mul(WorldToObject, float4(RayOrigin, 1.0));
    float3 localRayDir = mul((float3x3)WorldToObject, RayDirection);

    float t0, t1;
    IntersectAABB(localRayOrigin, localRayDir, Grid.LocalMin, Grid.LocalMax, t0, t1);
    t0 = max(t0, TMin);
    t1 = min(t1, TMax);

    if (t0 < t1) {
        uint bindlessIndex = Grid.TextureBindlessIndex;
        Texture3D<float4> densityTex = GetBindlessVolumeSRV(bindlessIndex);

        float3 boxSize = Grid.LocalMax - Grid.LocalMin;
        float3 invBoxSize = 1.0f / boxSize;

        float3 uvwOrigin = (RayOrigin - Grid.LocalMin) * invBoxSize;
        float3 uvwDirection = RayDirection * invBoxSize;

        float densityScale = 1.0f;
        float majorant = CalculateMaxDensityDDA(densityTex, uvwOrigin, uvwDirection, t0, t1, densityScale);

        float t = t0;
        float transmittance = 1.0f;
#ifdef USE_RAY_LIST
        uint RayListIndex = DispatchRaysIndex().x;
        uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
        uint RayIndex = DispatchRaysIndex().x;
#endif
        Random rng = MakeRandom(RayIndex + 0x8f71a213u, UB.Seed);

        while (true) {
            t -= log(1.0f - rng.rand()) / majorant;
            if (t >= t1) break;

            float3 pos = localRayOrigin + localRayDir * t;
            float3 uvw = (pos - Grid.LocalMin) / boxSize;

            float density = densityTex.SampleLevel(LinearWrapSampler, uvw, 0).a * densityScale;
            float nullProb = 1.0f - (density / majorant);
            transmittance *= nullProb;

            if (transmittance < 0.001f) {
                transmittance = 0.0f;
                break;
            }
        }

        Payload.Transmittance *= transmittance;
        if (Payload.Transmittance < 0.001f) {
            AcceptHitAndEndSearch();
        }
    }

    IgnoreHit();
}

// StaticMesh closesthit: record hit distance and zero transmittance (opaque hit)
[shader("closesthit")]
void TraceTransmittanceRaysClosestHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    // Found a static mesh instance. Return the hit distance.
    Payload.HitDistance = RayTCurrent();
    Payload.Transmittance = 0;
}

// Non-StaticMesh closesthit: ray was early terminated in anyhit, just zero transmittance
[shader("closesthit")]
void TraceTransmittanceRaysClosestHit_VolumePrimitives(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    // Otherwise the ray is early terminated in anyhit shader.
    // Simply set the transmittance to 0 and return.
    Payload.Transmittance = 0;
}

[shader("closesthit")]
void TraceTransmittanceRaysClosestHit_GaussianRadianceField(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    Payload.Transmittance = 0;
}

[shader("closesthit")]
void TraceTransmittanceRaysClosestHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    Payload.Transmittance = 0;
}

// GigaVoxel: VC chunk geometry with atlas-sampled opacity. anyHit accumulates
// transmittance for semi-transparent atlas texels; closestHit is fully opaque.
[shader("anyhit")]
void TraceTransmittanceRaysAnyHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                               BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    IntersectionMaterial Intersection = EvaluateGigaVoxelRenderableIntersectionMaterial(InstanceID(), PrimitiveIndex(), Attributes.barycentrics);
    // Intersection.Opacity is the atlas alpha; transmittance loss = (1 - opacity).
    Payload.Transmittance *= saturate(1.f - Intersection.Opacity);
    if(Intersection.Opacity < 0.99f) {
        IgnoreHit();
    }
}
[shader("closesthit")]
void TraceTransmittanceRaysClosestHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    Payload.HitDistance = RayTCurrent();
    Payload.Transmittance = 0;
}
