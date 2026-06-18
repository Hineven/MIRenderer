#ifndef GIGA_VOXEL_RESOURCES_HLSL
#define GIGA_VOXEL_RESOURCES_HLSL

// NOTE: This header only declares the GigaVoxel-specific buffers + the hit
// evaluation helper. It intentionally does NOT include RenderableResources /
// CommonSamplerResources / BindlessTextureResources, because the host shader
// already includes those (re-including would cause redefinition errors). The
// host shader MUST include this AFTER those so that RenderableHeaderBuffer,
// GetBindlessSRV, LinearWrapSampler etc. are already visible.
#include "../shared/SharedGigaVoxel.hlsl"
#include "../shared/SharedRenderable.hlsl"
#include "../headers/Intersection.hlsl"

// Global GigaVoxel vertex/index uber buffers (shared by ALL GigaVoxel assets,
// analogous to the StaticMesh VertexBuffer/IndexBuffer in GeometryResources.hlsl).
// GigaVoxelHeader.{Vertex,Index}Offset index into these.
StructuredBuffer<GigaVoxelVertex> GigaVoxelVertexBuffer;
StructuredBuffer<uint>            GigaVoxelIndexBuffer;
StructuredBuffer<GigaVoxelHeader> GigaVoxelHeaderBuffer;

// =============================================================================
// Evaluate a GigaVoxel renderable hit into an IntersectionMaterial.
//
// `Instance` is the renderable index (InstanceID() & INSTANCE_CUSTOM_INDEX_INDEX_MASK).
// The helper resolves it to a GigaVoxel asset index via RenderableHeaderBuffer +
// GigaVoxelInstanceHeader, then reads the chunk geometry + samples the atlas.
// Requires RenderableHeaderBuffer to be visible (declared by the host shader).
//
// GigaVoxel geometry is greedy-meshed VC chunk triangles with world-space
// positions (no per-instance transform) and a block-texture atlas. Atlas UV
// convention (see SharedGigaVoxel.hlsl):
//   finalUv = uv_base + frac(localUv * uv_scale) * (1 / 256)
// =============================================================================
IntersectionMaterial EvaluateGigaVoxelRenderableIntersectionMaterial (
    uint Instance,       // Renderable index (InstanceID() & INDEX_MASK)
    uint PrimitiveIndex, // Triangle index in the chunk BLAS
    float2 Barycentrics  // Intersection barycentrics
) {
    IntersectionMaterial Intersection = (IntersectionMaterial)0;

    uint GigaVoxelIndex = GetGigaVoxelInstanceHeader(RenderableHeaderBuffer[Instance]).GigaVoxelIndex;
    GigaVoxelHeader GV = GigaVoxelHeaderBuffer[GigaVoxelIndex];

    // Resolve the triangle's 3 indices (chunk-local indices rebased to global uber buffer).
    uint IndexOffset = GV.IndexOffset + PrimitiveIndex * 3;
    uint VertexOffset = GV.VertexOffset;
    uint IA = VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 0];
    uint IB = VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 1];
    uint IC = VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 2];

    GigaVoxelVertex VA = GigaVoxelVertexBuffer[IA];
    GigaVoxelVertex VB = GigaVoxelVertexBuffer[IB];
    GigaVoxelVertex VC = GigaVoxelVertexBuffer[IC];

    // Barycentric interpolation (weights: A=1-bary.x-bary.y, B=bary.x, C=bary.y).
    float w0 = 1.0 - Barycentrics.x - Barycentrics.y;
    float w1 = Barycentrics.x;
    float w2 = Barycentrics.y;

    // Positions are already world-space (greedy mesher emits world coords).
    float3 Position = VA.position * w0 + VB.position * w1 + VC.position * w2;
    Intersection.LocalPosition = Position;
    Intersection.WorldPosition = Position;

    // Geometry normal from the face (axis-aligned face normal from greedy mesh).
    float3 GeoNormal = normalize(cross(VB.position - VA.position, VC.position - VA.position));
    Intersection.GeometryNormal = GeoNormal;
    Intersection.ShadingNormal = normalize(VA.normal * w0 + VB.normal * w1 + VC.normal * w2);

    // Interpolate atlas-local UV, then map into the atlas tile.
    float2 LocalUV = VA.uv_base * w0 + VB.uv_base * w1 + VC.uv_base * w2;
    float2 UVScale = VA.uv_scale; // uniform per quad
    float2 AtlasUV = LocalUV + frac(LocalUV * UVScale) * (1.0 / 256.0);

    // Sample the block atlas (global bindless texture). RGB = albedo, A = opacity.
    Intersection.Albedo = 1.0;
    Intersection.Opacity = 1.0;
    if (GV.AtlasBindlessIndex != 0xFFFFFFFFu) {
        float4 AlbedoOpacity = GetBindlessSRV(GV.AtlasBindlessIndex).SampleLevel(LinearWrapSampler, AtlasUV, 0);
        Intersection.Albedo = AlbedoOpacity.rgb;
        Intersection.Opacity = AlbedoOpacity.a;
    }

    // GigaVoxel blocks: no emission, no PBR metalness (treat as diffuse dielectric).
    Intersection.Emission = 0;
    Intersection.MetallicRoughness = float2(0.0, 1.0);
    Intersection.bDoubleSided = true;

    return Intersection;
}

#endif
