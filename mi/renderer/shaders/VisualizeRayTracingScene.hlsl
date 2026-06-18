#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedVertex.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/LightGrid.hlsl"
#include "resources/EnvironmentLightResource.hlsl"

struct VisualizeRayTracingSceneUB {
    float3 EnvironmentMapMultiplier;
    float EnvironmentMapLOD;
};
ConstantBuffer<VisualizeRayTracingSceneUB> UB;

RaytracingAccelerationStructure PTLAS;

StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;
StructuredBuffer<PackedVolumePrimitive> PrimitiveData;

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDebugOutput;


struct [raypayload] RayPayload {
    float4 Color;
    float Transmittance; // For volume primitives
    float THit;
    bool bSurfaceHit;
};

#include "resources/GigaVoxelResources.hlsl"
[shader("raygeneration")]
void VisualizeRayTracingSceneRaygen() {

    uint2 RayIndex = DispatchRaysIndex().xy;
    uint2 DispatchSize = DispatchRaysDimensions().xy;

    RayDesc Ray = (RayDesc)0;
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

    RayPayload Payload = (RayPayload)0;
    Payload.Transmittance = 1.0f;
    for(int i = 0; i < 100; i++) {
        Payload.THit = Ray.TMax;
        TraceRay(
        PTLAS,
            RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
            0xFF, // Ray mask
            0,    // SBT offset
            0,    // SBT stride (per-instance offset only)
            0,    // Miss shader index
            Ray,
            Payload
        );
        Ray.TMin = Payload.THit + 1e-4f;
        if(Payload.bSurfaceHit || Payload.THit == Ray.TMax) break;
    }
    RWDebugOutput[RayIndex] = Payload.Color * Payload.Transmittance;
}

[shader("miss")]
void VisualizeRayTracingSceneMiss(inout RayPayload Payload: SV_RayPayload) {
    float3 RayDirection = WorldRayDirection();
    float3 EnvironmentColor = EvaluateEnvironmentMap_Raw(
        -RayDirection, 
        UB.EnvironmentMapLOD,
        UB.EnvironmentMapMultiplier
    );
    Payload.Color = float4(EnvironmentColor, 1.0f);
}

// ============================================================================
// StaticMesh anyhit: alpha test
// ============================================================================
[shader("anyhit")]
void VisualizeRayTracingSceneAnyHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
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

    DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

    MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
    float4 ColorOpacity = float4(Material.Albedo, 1);
    if(IsValid(Material.AlbedoMap)) {
        ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
    }
    if(ColorOpacity.a < 0.01f) {
        IgnoreHit();
    }
}

// Non-StaticMesh anyhit: just pass through (shell geometry)
[shader("anyhit")]
void VisualizeRayTracingSceneAnyHit_VolumePrimitives(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
}

[shader("anyhit")]
void VisualizeRayTracingSceneAnyHit_GaussianRadianceField(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
}

[shader("anyhit")]
void VisualizeRayTracingSceneAnyHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
}

// ============================================================================
// StaticMesh closesthit: visualize material color
// ============================================================================
[shader("closesthit")]
void VisualizeRayTracingSceneClosestHit_StaticMesh(inout RayPayload Payload: SV_RayPayload,
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

    DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

    MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
    float4 ColorOpacity = float4(Material.Albedo, 1);
    if(IsValid(Material.AlbedoMap)) {
        ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
    }
    Payload.Color = ColorOpacity;
    Payload.bSurfaceHit = true;
    Payload.THit = RayTCurrent();
}

// Non-StaticMesh closesthit: reduce transmittance for shell geometry visualization
[shader("closesthit")]
void VisualizeRayTracingSceneClosestHit_VolumePrimitives(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    Payload.Transmittance *= 0.5f;
    Payload.THit = RayTCurrent();
}

[shader("closesthit")]
void VisualizeRayTracingSceneClosestHit_GaussianRadianceField(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    Payload.Transmittance *= 0.5f;
    Payload.THit = RayTCurrent();
}

[shader("closesthit")]
void VisualizeRayTracingSceneClosestHit_VolumeGrid(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    Payload.Transmittance *= 0.5f;
    Payload.THit = RayTCurrent();
}

// GigaVoxel: VC chunk geometry. Visualize the atlas-sampled albedo color.
[shader("anyhit")]
void VisualizeRayTracingSceneAnyHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                               BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
}
[shader("closesthit")]
void VisualizeRayTracingSceneClosestHit_GigaVoxel(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Instance = InstanceID() & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    IntersectionMaterial Intersection = EvaluateGigaVoxelRenderableIntersectionMaterial(Instance, PrimitiveIndex(), Attributes.barycentrics);
    Payload.Color = float4(Intersection.Albedo, Intersection.Opacity);
    Payload.bSurfaceHit = true;
    Payload.THit = RayTCurrent();
}
