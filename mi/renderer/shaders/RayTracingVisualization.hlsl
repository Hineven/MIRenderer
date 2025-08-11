#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedVertex.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"

RaytracingAccelerationStructure TLAS;

StructuredBuffer<StaticMeshInstanceHeader> RenderableHeaderBuffer;
StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<MaterialHeader> MaterialHeaderBuffer;

RWTexture2D<float4> RWDebugOutput;
TextureCube<float4> EnvironmentMap;
SamplerState LinearSampler;

struct RayPayload {
    float4 Color;
};

[shader("raygeneration")]
void RayTracingVisualizationRaygen() {

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
    TraceRay(
        TLAS,
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF, // Ray mask
        0,    // SBT offset
        0,    // SBT stride
        0,    // Miss shader index
        Ray,
        Payload
    );
    RWDebugOutput[RayIndex] = Payload.Color;
}

[shader("miss")]
void RayTracingVisualizationMiss(inout RayPayload Payload: SV_RayPayload) {
    float3 RayDirection = WorldRayDirection();
    float3 EnvironmentColor = EnvironmentMap.SampleLevel(LinearSampler, -RayDirection, 0).xyz;
    Payload.Color = float4(EnvironmentColor, 1.0f);
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
void RayTracingVisualizationAnyHit(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    if(InstanceFlags == 0) {
        StaticMeshInstanceHeader InstanceHeader = RenderableHeaderBuffer[Instance];
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
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearSampler, InterpolatedVertex.UV, 0);
        }
	    if(ColorOpacity.a < 0.1f) {
 		    IgnoreHit();
	    }
    }
}

[shader("closesthit")]
void RayTracingVisualizationClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    if(InstanceFlags == 0) {
        StaticMeshInstanceHeader InstanceHeader = RenderableHeaderBuffer[Instance];
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
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearSampler, InterpolatedVertex.UV, 0);
        }
        Payload.Color = ColorOpacity;
    } else {
        Payload.Color = float4(1.0f, 1.0f, 0.0f, 1.0f); // Red color for volume primitives
    }
}