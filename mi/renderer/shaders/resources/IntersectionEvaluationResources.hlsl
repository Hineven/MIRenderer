#ifndef INTERSECTION_EVALUATION_RESOURCES_HLSL
#define INTERSECTION_EVALUATION_RESOURCES_HLSL

#include "../headers/Conventions.hlsl"
#include "../headers/Intersection.hlsl"
#include "../shared/SharedStaticMesh.hlsl"
#include "../headers/Transform.hlsl"
#include "RenderableResources.hlsl"
#include "CommonSamplerResources.hlsl"
#include "StaticMeshResources.hlsl"
#include "BindlessTextureResources.hlsl"
#include "MaterialResources.hlsl"
#include "../headers/TextureSampling.hlsl"

IntersectionMaterial EvaluateStaticMeshRenderableIntersectionMaterial_InputTransforms (
    uint RenderableIndex, // Renderable index, must be a static mesh renderable
    uint DescriptorRank, // Descriptor rank of the static mesh this renderable refers to
    uint PrimitiveIndex, // Triangle index of the geometry
    float2 Barycentrics, // Intersection triangle barycentrics
    float3x4 RenderableTransform, // Transform matrix for the renderable
    float3x3 RenderableNormalTransform, // Normal transform matrix for the renderable
    float LOD = -1 // LOD level, -1 means automatic LOD selection (only works in PS)
) {
    IntersectionMaterial Intersection = (IntersectionMaterial)0;

    StaticMeshInstanceHeader InstanceHeader = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]);
    uint StaticMeshIndex = InstanceHeader.StaticMeshIndex;
    uint DescriptionOffset = StaticMeshHeaderBuffer[StaticMeshIndex].DescriptionOffset;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[DescriptionOffset + DescriptorRank];
    uint GeometryIndex = GeometryMaterialPair.x;
    uint MaterialIndex = GeometryMaterialPair.y;
    GeometryHeader Geometry = GeometryHeaderBuffer[GeometryIndex];
    uint IndexOffset = Geometry.IndexOffset + PrimitiveIndex * 3;
    uint VertexOffset = Geometry.VertexOffset;

    uint VertexAIndex = VertexOffset + IndexBuffer[IndexOffset + 0];
    uint VertexBIndex = VertexOffset + IndexBuffer[IndexOffset + 1];
    uint VertexCIndex = VertexOffset + IndexBuffer[IndexOffset + 2];
    DefaultStaticMeshVertex VertexA = VertexBuffer[VertexAIndex];
    DefaultStaticMeshVertex VertexB = VertexBuffer[VertexBIndex];
    DefaultStaticMeshVertex VertexC = VertexBuffer[VertexCIndex];

    // Interpolate the vertex
    DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Barycentrics);
    float3 LocalNormal = normalize(cross(VertexB.Position - VertexA.Position, VertexC.Position - VertexA.Position));
    Intersection.GeometryNormal = normalize(mul(RenderableNormalTransform, LocalNormal));
    
    // Transform to world space
    float3x4 ToWorldTransform = RenderableTransform;
    Intersection.LocalPosition = InterpolatedVertex.Position;
    Intersection.WorldPosition = TransformPoint(ToWorldTransform, InterpolatedVertex.Position);

    MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];

    bool bPointSampled = Material.Flags & MATERIAL_FLAG_POINT_SAMPLED;

    Intersection.Albedo = Material.Albedo;
    Intersection.Opacity = 1;
    Intersection.MetallicRoughness = float2(Material.Metallic, Material.Roughness);
    Intersection.Emission = Material.Emissive;

    // Albedo texture
    if(IsValid(Material.AlbedoMap)) {
        float4 AlbedoOpacity = 0;
        if(bPointSampled) {
            AlbedoOpacity = SampleTexture(GetBindlessSRV(Material.AlbedoMap), PointWrapSampler, InterpolatedVertex.UV, LOD);
        } else {
            AlbedoOpacity = SampleTexture(GetBindlessSRV(Material.AlbedoMap), LinearWrapSampler, InterpolatedVertex.UV, LOD);
        }
        Intersection.Albedo = AlbedoOpacity.rgb;
        Intersection.Opacity = AlbedoOpacity.a;
    }

    // Reconstruct shading normal
    float3x3 NormalTransform  = RenderableNormalTransformBuffer[RenderableIndex];
    Intersection.ShadingNormal = normalize(mul(NormalTransform, InterpolatedVertex.Normal));
    if(IsValid(Material.NormalMap)) {
        float3 PosA = TransformPoint(ToWorldTransform, VertexA.Position);
        float3 PosB = TransformPoint(ToWorldTransform, VertexB.Position);
        float3 PosC = TransformPoint(ToWorldTransform, VertexC.Position);
        float2 UV_A = VertexA.UV;
        float2 UV_B = VertexB.UV;
        float2 UV_C = VertexC.UV;

        float3 EdgePos1 = PosB - PosA;
        float3 EdgePos2 = PosC - PosA;
        float2 EdgeUV1 = UV_B - UV_A;
        float2 EdgeUV2 = UV_C - UV_A;

        // TBN matrix
        // http://www.opengl-tutorial.org/intermediate-tutorials/tutorial-13-normal-mapping/
        float r = 1.0f / (EdgeUV1.x * EdgeUV2.y - EdgeUV1.y * EdgeUV2.x);
        float3 Tangent   = normalize((EdgePos1 * EdgeUV2.y - EdgePos2 * EdgeUV1.y) * r);
        float3 Bitangent = normalize((EdgePos2 * EdgeUV1.x - EdgePos1 * EdgeUV2.x) * r);
        
        // Making the tangent ortho to the interpolated vertex normal (Gram-Schmidt process)
        Tangent = normalize(Tangent - dot(Tangent, Intersection.ShadingNormal) * Intersection.ShadingNormal);
        float Handedness = dot(cross(Intersection.ShadingNormal, Tangent), Bitangent) < 0.0f ? -1.0f : 1.0f;
        Bitangent = cross(Intersection.ShadingNormal, Tangent) * Handedness;

        // Sample normal map and reconstruct shading normal
        float3 NormalMapSample;
        
        if (bPointSampled) {
            NormalMapSample = SampleTexture(GetBindlessSRV(Material.NormalMap), PointWrapSampler, InterpolatedVertex.UV, LOD).xyz * 2.0f - 1.0f;
        } else {
            NormalMapSample = SampleTexture(GetBindlessSRV(Material.NormalMap), LinearWrapSampler, InterpolatedVertex.UV, LOD).xyz * 2.0f - 1.0f;
        } 
        Intersection.ShadingNormal = normalize(
            NormalMapSample.x * Tangent +
            NormalMapSample.y * Bitangent +
            NormalMapSample.z * Intersection.ShadingNormal
        );
    }

    // Emission texture
    if(IsValid(Material.EmissiveMap)) {
        float4 EmissionA;
        if (bPointSampled) {
            EmissionA = SampleTexture(GetBindlessSRV(Material.EmissiveMap), PointWrapSampler, InterpolatedVertex.UV, LOD);
        } else {
            EmissionA = SampleTexture(GetBindlessSRV(Material.EmissiveMap), LinearWrapSampler, InterpolatedVertex.UV, LOD);
        }
        // Regular GLTF ignores A channel of emissive maps.
        // TODO make use of that channel in our custom material format.
        Intersection.Emission = EmissionA.rgb;
    }

    // MetallicRoughness texture
    if(IsValid(Material.MetallicRoughnessMap)) {
        if (bPointSampled) {
            Intersection.MetallicRoughness = SampleTexture(GetBindlessSRV(Material.MetallicRoughnessMap), PointWrapSampler, InterpolatedVertex.UV, LOD).xy;
        } else {
            Intersection.MetallicRoughness = SampleTexture(GetBindlessSRV(Material.MetallicRoughnessMap), LinearWrapSampler, InterpolatedVertex.UV, LOD).xy;
        }
    }
    Intersection.bDoubleSided = Material.Flags & MATERIAL_FLAG_DOUBLE_SIDED;
    return Intersection;
}

IntersectionMaterial EvaluateStaticMeshRenderableIntersectionMaterial (
    uint RenderableIndex, // Renderable index, must be a static mesh renderable
    uint DescriptorRank, // Descriptor rank of the static mesh this renderable refers to
    uint PrimitiveIndex, // Triangle index of the geometry
    float2 Barycentrics, // Intersection triangle barycentrics
    float LOD = -1 // LOD level, -1 means automatic LOD selection (only works in PS)
) {
    return EvaluateStaticMeshRenderableIntersectionMaterial_InputTransforms(
        RenderableIndex,
        DescriptorRank,
        PrimitiveIndex,
        Barycentrics,
        RenderableTransformBuffer[RenderableIndex],
        RenderableNormalTransformBuffer[RenderableIndex],
        LOD
    );
}

#endif