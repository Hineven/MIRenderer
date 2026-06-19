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
// Per-chunk geometry addressing lives in GigaVoxelChunkHeaderBuffer.
StructuredBuffer<GigaVoxelVertex>      GigaVoxelVertexBuffer;
StructuredBuffer<uint>                 GigaVoxelIndexBuffer;
StructuredBuffer<GigaVoxelHeader>      GigaVoxelHeaderBuffer;
// Per-chunk geometry header (one row per chunk). Indexed by the global chunk
// index encoded in the RT instance_custom_index high 12 bits / visibility y.
StructuredBuffer<GigaVoxelChunkHeader> GigaVoxelChunkHeaderBuffer;

// =============================================================================
// Decode a GigaVoxel RT instance_custom_index into (RenderableIndex, ChunkIndex).
//
// Layout (set in GigaVoxel::RebuildCachedInstances):
//   [GlobalChunkIndex:12bits][RenderableIndex:20bits]
// =============================================================================
void DecodeGigaVoxelInstanceCustomIndex(uint InstanceCustomIndex,
                                        out uint RenderableIndex,
                                        out uint ChunkIndex) {
    RenderableIndex = InstanceCustomIndex & 0xFFFFFu;
    ChunkIndex      = (InstanceCustomIndex >> 20) & 0xFFFu;
}

// =============================================================================
// Evaluate a GigaVoxel RT hit into an IntersectionMaterial.
//
// `InstanceCustomIndex` is the raw InstanceID() value (NOT pre-masked): it
// encodes [GlobalChunkIndex:12][RenderableIndex:20] (see
// DecodeGigaVoxelInstanceCustomIndex). The helper:
//   1. Resolves the asset via RenderableIndex -> RenderableHeader -> GigaVoxel
//      -> per-asset GigaVoxelHeader (carries the global atlas bindless index).
//   2. Resolves the chunk geometry via ChunkIndex -> GigaVoxelChunkHeaderBuffer
//      -> {vertex_offset, index_offset} in the global uber buffers.
//   3. Reads the chunk-local triangle (PrimitiveIndex is 0-based per BLAS) and
//      barycentrically interpolates, then samples the block atlas.
//
// `PrimitiveIndex` is the RT builtin (chunk-local, 0-based). Geometry is
// greedy-meshed VC chunk triangles with world-space positions (no per-instance
// transform) and the block-texture atlas. Atlas UV convention:
//   finalUv = uv_base + frac(localUv * uv_scale) * (1 / 256)
// =============================================================================
IntersectionMaterial EvaluateGigaVoxelRenderableIntersectionMaterial (
    uint InstanceCustomIndex, // raw InstanceID() (encodes RenderableIndex + ChunkIndex)
    uint PrimitiveIndex,      // chunk-local triangle index (RT builtin)
    float2 Barycentrics       // intersection barycentrics
) {
    IntersectionMaterial Intersection = (IntersectionMaterial)0;

    uint RenderableIndex, ChunkIndex;
    DecodeGigaVoxelInstanceCustomIndex(InstanceCustomIndex, RenderableIndex, ChunkIndex);

    // Asset-level: atlas bindless index.
    uint GigaVoxelIndex = GetGigaVoxelInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).GigaVoxelIndex;
    GigaVoxelHeader GV = GigaVoxelHeaderBuffer[GigaVoxelIndex];

    // Chunk-level: geometry span in the global uber buffers.
    GigaVoxelChunkHeader CH = GigaVoxelChunkHeaderBuffer[ChunkIndex];

    // Resolve the triangle's 3 indices (PrimitiveIndex is chunk-local, 0-based;
    // CH.IndexOffset rebases it into the global index uber buffer).
    uint IndexOffset = CH.IndexOffset + PrimitiveIndex * 3;
    uint IA = CH.VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 0];
    uint IB = CH.VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 1];
    uint IC = CH.VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 2];

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

// =============================================================================
// Evaluate a GigaVoxel visibility-buffer pixel into an IntersectionMaterial.
//
// Visibility payload (GigaVoxel VC, per gigavoxel_visibility_buffer.md §3.2):
//   x[19:0]  RenderableIndex, x[31:20] RenderableType (= kGigaVoxel)
//   y        GlobalChunkIndex
//   z[15:0]  PrimitiveIndex (chunk-local), z[31:16] reserved
//   w        Barycentrics.z (f32, NON-NaN -> VC branch; NaN would mean LFC)
//
// Bary.x = 1 - Bary.y - Bary.z (derived). This mirrors the RT path but resolves
// the chunk from the payload's GlobalChunkIndex instead of InstanceID().
// =============================================================================
IntersectionMaterial EvaluateGigaVoxelVisibilityIntersectionMaterial (
    uint RenderableIndex,
    uint GlobalChunkIndex,
    uint PrimitiveIndex,
    float2 Barycentrics   // (.y, .z); .x derived
) {
    IntersectionMaterial Intersection = (IntersectionMaterial)0;

    // Asset-level: atlas bindless index.
    uint GigaVoxelIndex = GetGigaVoxelInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).GigaVoxelIndex;
    GigaVoxelHeader GV = GigaVoxelHeaderBuffer[GigaVoxelIndex];

    // Chunk-level: geometry span in the global uber buffers.
    GigaVoxelChunkHeader CH = GigaVoxelChunkHeaderBuffer[GlobalChunkIndex];

    // Resolve the triangle's 3 indices (PrimitiveIndex is chunk-local, 0-based).
    uint IndexOffset = CH.IndexOffset + PrimitiveIndex * 3;
    uint IA = CH.VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 0];
    uint IB = CH.VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 1];
    uint IC = CH.VertexOffset + GigaVoxelIndexBuffer[IndexOffset + 2];

    GigaVoxelVertex VA = GigaVoxelVertexBuffer[IA];
    GigaVoxelVertex VB = GigaVoxelVertexBuffer[IB];
    GigaVoxelVertex VC = GigaVoxelVertexBuffer[IC];

    // Barycentric interpolation (weights: A=1-bary.x-bary.y, B=bary.x, C=bary.y).
    float w0 = 1.0 - Barycentrics.x - Barycentrics.y;
    float w1 = Barycentrics.x;
    float w2 = Barycentrics.y;

    float3 Position = VA.position * w0 + VB.position * w1 + VC.position * w2;
    Intersection.LocalPosition = Position;
    Intersection.WorldPosition = Position;

    float3 GeoNormal = normalize(cross(VB.position - VA.position, VC.position - VA.position));
    Intersection.GeometryNormal = GeoNormal;
    Intersection.ShadingNormal = normalize(VA.normal * w0 + VB.normal * w1 + VC.normal * w2);

    float2 LocalUV = VA.uv_base * w0 + VB.uv_base * w1 + VC.uv_base * w2;
    float2 UVScale = VA.uv_scale;
    float2 AtlasUV = LocalUV + frac(LocalUV * UVScale) * (1.0 / 256.0);

    Intersection.Albedo = 1.0;
    Intersection.Opacity = 1.0;
    if (GV.AtlasBindlessIndex != 0xFFFFFFFFu) {
        float4 AlbedoOpacity = GetBindlessSRV(GV.AtlasBindlessIndex).SampleLevel(LinearWrapSampler, AtlasUV, 0);
        Intersection.Albedo = AlbedoOpacity.rgb;
        Intersection.Opacity = AlbedoOpacity.a;
    }

    Intersection.Emission = 0;
    Intersection.MetallicRoughness = float2(0.0, 1.0);
    Intersection.bDoubleSided = true;

    return Intersection;
}

#endif
