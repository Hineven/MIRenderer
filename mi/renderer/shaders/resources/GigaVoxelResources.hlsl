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
#include "../headers/Transform.hlsl"

// Global GigaVoxel vertex/index uber buffers (shared by ALL GigaVoxel assets,
// analogous to the StaticMesh VertexBuffer/IndexBuffer in GeometryResources.hlsl).
// Per-chunk geometry addressing lives in GigaVoxelChunkHeaderBuffer.
StructuredBuffer<GigaVoxelVertex>      GigaVoxelVertexBuffer;
StructuredBuffer<uint>                 GigaVoxelIndexBuffer;
StructuredBuffer<GigaVoxelHeader>      GigaVoxelHeaderBuffer;
// Per-chunk geometry header (one row per chunk). Indexed by the chunk_header_index
// in the low 16 bits of the RT instance_custom_index / visibility payload y.
// Carries ChunkOrigin: the chunk-local -> world translation for chunk-local verts.
StructuredBuffer<GigaVoxelChunkHeader> GigaVoxelChunkHeaderBuffer;
// Per-instance RT header side table (256 entries). Indexed by the high 8 bits of
// a GigaVoxel RT instance_custom_index; each row holds the instance's
// RenderableIndex + GigaVoxelIndex. See SharedGigaVoxel.hlsl.
StructuredBuffer<GigaVoxelInstanceRTHeader> GigaVoxelInstanceRTHeaderBuffer;

// =============================================================================
// Decode a GigaVoxel RT instance_custom_index into (RTHeaderIndex, ChunkIndex).
//
// Layout (set in GigaVoxelInstance::RebuildCachedInstances):
//   [RTHeaderIndex:8bits 16-23][chunk_header_index:16bits 0-15]
// RTHeaderIndex indexes GigaVoxelInstanceRTHeaderBuffer above.
// =============================================================================
void DecodeGigaVoxelInstanceCustomIndex(uint InstanceCustomIndex,
                                        out uint RTHeaderIndex,
                                        out uint ChunkIndex) {
    RTHeaderIndex = (InstanceCustomIndex >> 16) & 0xFFu;
    ChunkIndex    = InstanceCustomIndex & 0xFFFFu;
}

// =============================================================================
// Evaluate a GigaVoxel RT hit into an IntersectionMaterial.
//
// `InstanceCustomIndex` is the raw InstanceID() value (NOT pre-masked): it
// encodes [RTHeaderIndex:8][chunk_header_index:16] (see
// DecodeGigaVoxelInstanceCustomIndex). The helper:
//   1. Resolves the asset via RenderableIndex -> RenderableHeader -> GigaVoxel
//      -> per-asset GigaVoxelHeader (carries the global atlas bindless index).
//   2. Resolves the chunk geometry via ChunkIndex -> GigaVoxelChunkHeaderBuffer
//      -> {vertex_offset, index_offset} in the global uber buffers.
//   3. Reads the chunk-local triangle (PrimitiveIndex is 0-based per BLAS) and
//      barycentrically interpolates, then samples the block atlas.
//
// `PrimitiveIndex` is the RT builtin (chunk-local, 0-based). Vertices are in
// CHUNK-LOCAL space; the per-chunk TLAS instance transform (set in
// GigaVoxelInstance::RebuildCachedInstances = the chunk world origin) maps them
// to world space. The caller passes ObjectToWorld3x4() / WorldToObject3x4()
// (RT builtins, valid only at the hit-shader entry point). Atlas UV convention:
//   finalUv = uv_base + frac(localUv * uv_scale) * (1 / 256)
// =============================================================================
IntersectionMaterial EvaluateGigaVoxelRenderableIntersectionMaterial (
    uint InstanceCustomIndex, // raw InstanceID() (encodes RenderableIndex + ChunkIndex)
    uint PrimitiveIndex,      // chunk-local triangle index (RT builtin)
    float2 Barycentrics,      // intersection barycentrics
    float3x4 ObjectToWorld,   // chunk-local -> world (RT builtin ObjectToWorld3x4())
    float3x3 NormalTransform  // world-space normal basis = transpose(WorldToObject3x4())
) {
    IntersectionMaterial Intersection = (IntersectionMaterial)0;

    uint RTHeaderIndex, ChunkIndex;
    DecodeGigaVoxelInstanceCustomIndex(InstanceCustomIndex, RTHeaderIndex, ChunkIndex);

    // Asset-level: atlas bindless index. RTHeaderBuffer carries this instance's
    // GigaVoxelIndex directly (no detour through RenderableHeaderBuffer, unlike
    // the visibility path).
    GigaVoxelInstanceRTHeader RT = GigaVoxelInstanceRTHeaderBuffer[RTHeaderIndex];
    GigaVoxelHeader GV = GigaVoxelHeaderBuffer[RT.GigaVoxelIndex];

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

    // Vertices are chunk-local; ObjectToWorld (per-chunk TLAS instance transform)
    // maps them to world space.
    float3 LocalPosition = VA.position * w0 + VB.position * w1 + VC.position * w2;
    Intersection.LocalPosition = LocalPosition;
    Intersection.WorldPosition = TransformPoint(ObjectToWorld, LocalPosition);

    // Geometry normal from the face (axis-aligned face normal from greedy mesh).
    // Transform to world via the caller-supplied normal basis (a pure
    // translation leaves the normal unchanged).
    float3 LocalGeoNormal = normalize(cross(VB.position - VA.position, VC.position - VA.position));
    Intersection.GeometryNormal = normalize(TransformVector(NormalTransform, LocalGeoNormal));
    float3 LocalShadingNormal = normalize(VA.normal * w0 + VB.normal * w1 + VC.normal * w2);
    Intersection.ShadingNormal = normalize(TransformVector(NormalTransform, LocalShadingNormal));

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
//
// Vertices are chunk-local; `ObjectToWorld` is the chunk-local -> world transform
// (a pure translation = the chunk's world origin from GigaVoxelChunkHeader), as
// the raster path has no ObjectToWorld3x4() builtin. The caller builds it from
// GigaVoxelChunkHeaderBuffer[GlobalChunkIndex].ChunkOrigin.
// =============================================================================
IntersectionMaterial EvaluateGigaVoxelVisibilityIntersectionMaterial (
    uint RenderableIndex,
    uint GlobalChunkIndex,
    uint PrimitiveIndex,
    float2 Barycentrics,    // (.y, .z); .x derived
    float3x4 ObjectToWorld  // chunk-local -> world (chunk origin translation)
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

    float3 LocalPosition = VA.position * w0 + VB.position * w1 + VC.position * w2;
    Intersection.LocalPosition = LocalPosition;
    Intersection.WorldPosition = TransformPoint(ObjectToWorld, LocalPosition);

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
