// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_GIGA_VOXEL_HLSL
#define MI_RENDERER_SHADERS_SHARED_GIGA_VOXEL_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// =============================================================================
// GigaVoxel device-side header (per-asset).
//
// One GigaVoxel asset occupies a single bindless slot in the
// DeviceBindlessResourceAllocator; this header (stored in the
// GigaVoxelHeaderBuffer, indexed by the slot) carries the asset-level
// block-texture atlas bindless index (the atlas is process-global, shared by
// ALL GigaVoxel assets, like an MC resource pack). Per-chunk geometry
// addressing lives in GigaVoxelChunkHeader (one row per chunk, indexed by a
// global chunk index).
//
// NOTE: VertexOffset/IndexOffset/VertexCount/IndexCount below are the legacy
// Phase-1 merged-range fields (covering the whole heap watermark) and are now
// SUPERSEDED by the per-chunk GigaVoxelChunkHeader for geometry addressing.
// They remain for backward compatibility / transitional code; new code should
// resolve geometry via GigaVoxelChunkHeaderBuffer. AtlasBindlessIndex is the
// only field still actively read here.
// =============================================================================
struct GigaVoxelHeader {
    uint VertexOffset;        // [deprecated, see GigaVoxelChunkHeader] legacy merged-range offset
    uint IndexOffset;         // [deprecated, see GigaVoxelChunkHeader] legacy merged-range offset
    uint VertexCount;         // [deprecated, see GigaVoxelChunkHeader] legacy merged-range count
    uint IndexCount;          // [deprecated, see GigaVoxelChunkHeader] legacy merged-range count
    uint AtlasBindlessIndex;  // Bindless descriptor index of the 4096^2 block atlas (SRV, global)
    uint3 _Padding;
};

// =============================================================================
// GigaVoxelChunkHeader (per-chunk, geometry addressing + world placement).
//
// One row per chunk, stored in the global GigaVoxelChunkHeaderBuffer (owned by
// GigaVoxelGeometryHeap), indexed by a stable global chunk index assigned at
// AllocateChunk time. This is what lets a visibility-buffer pixel / RT hit
// recover the chunk's vertex/index span in the global uber buffers from just a
// chunk index — the RT instance_custom_index encodes (RenderableIndex | chunk
// index << 20), and the visibility-buffer payload stores the chunk index.
//
// Offsets follow the same convention as elsewhere:
//   - VertexOffset is measured in elements (sizeof(GigaVoxelVertex) units).
//   - IndexOffset  is measured in elements (sizeof(uint32) units).
//
// ChunkOrigin is the chunk-local -> world translation: vertices are stored in
// CHUNK-LOCAL space (origin at the chunk's min corner), and ChunkOrigin is the
// world position of that corner. The RT path gets this automatically via
// ObjectToWorld3x4() (the per-chunk TLAS instance transform); the raster /
// visibility path reads ChunkOrigin from here to place chunk-local vertices
// into world space.
//
// Atlas is intentionally NOT here: the block atlas is process-global (one
// MC-resource-pack), carried per-asset in GigaVoxelHeader.AtlasBindlessIndex.
// =============================================================================
struct GigaVoxelChunkHeader {
    uint VertexOffset;        // Offset into the vertex uber buffer (in elements)
    uint IndexOffset;         // Offset into the index  uber buffer (in elements)
    uint VertexCount;
    uint IndexCount;
    float3 ChunkOrigin;       // World position of the chunk's local-space origin
    uint _Padding0;           // explicit pad -> 32 bytes, 16-byte aligned
};

// =============================================================================
// GigaVoxelVertex: GPU-friendly vertex format for VC (Vanilla Chunk) geometry.
//
// Mirrors macromc::meshing::VoxelVertex field-for-field but lives in the
// renderer layer (no dependency on macromc) and is padded to a clean 48 bytes.
// The macromc host code converts VoxelVertex -> GigaVoxelVertex at upload time.
//
// Positions are in CHUNK-LOCAL space: the origin is the chunk's min corner, so
// a vertex at local (0,0,0) sits at the chunk's world placement
// (GigaVoxelChunkHeader.ChunkOrigin). World position = ChunkOrigin + position.
// The per-chunk world placement is carried by the TLAS instance transform
// (ObjectToWorld3x4() in RT) and by GigaVoxelChunkHeader.ChunkOrigin in the
// raster / visibility path.
//
// Atlas UV convention (see macromc meshing/types.h):
//   finalUv = uv_base + frac(localUv * uv_scale) * (1 / 256)
// keeps sampling inside one 16x16 tile (256x256 tile grid in a 4096^2 atlas).
// =============================================================================
struct GigaVoxelVertex {
    float3 position;        // chunk-local vertex position (meters; world = ChunkOrigin + position)
    float3 normal;          // axis-aligned face normal (one of +-X/+-Y/+-Z)
    float2 uv_base;         // atlas-space origin (bottom-left) of the block's tile
    float2 uv_scale;        // how many tiles the owning quad spans (u, v)
    uint   texture_index;   // block tile index (== BlockDefinition::texture_index)
    uint   _pad;            // explicit pad -> 48 bytes, 4-byte aligned
};

// =============================================================================
// GigaVoxelInstanceRTHeader (per-instance, RT-only side table).
//
// GigaVoxel is the only renderable type that packs type-specific data into the
// RT InstanceCustomIndex: the high 8 bits carry an index into this fixed-size
// (256-entry) side table, owned by the DeviceBindlessResourceAllocator. Each
// entry holds the GigaVoxelInstance's scene-level RenderableIndex (so RT hit
// shaders can still reach scene-level per-renderable buffers like Hash /
// PrevTransform) and the asset-level GigaVoxelIndex (so they can reach
// GigaVoxelHeaderBuffer for the atlas bindless index).
//
// This decouples the RT InstanceCustomIndex contract from every other
// renderable type (StaticMesh / VolumeGrid use the full 24 bits for the plain
// RenderableIndex). The RT shader decode path is:
//   InstanceCustomIndex -> [header_idx:8][chunk_header_idx:16]
//   GigaVoxelInstanceRTHeaderBuffer[header_idx] -> { RenderableIndex, GigaVoxelIndex }
//
// (The visibility-buffer raster path does NOT use this table: it already has
// the RenderableIndex from its per-draw upload and resolves the GigaVoxelIndex
// via RenderableHeaderBuffer[RenderableIndex], as for other renderable types.)
// =============================================================================
struct GigaVoxelInstanceRTHeader {
    uint RenderableIndex;  // -> RenderableHeaderBuffer / Hash / PrevTransform (scene-level)
    uint GigaVoxelIndex;   // -> GigaVoxelHeaderBuffer (asset-level atlas)
};

// Bit layout for the GigaVoxel RT InstanceCustomIndex.
// [GigaVoxelInstanceRTHeaderIndex:8 bits 16-23][chunk_header_index:16 bits 0-15].
// These constants are shared between C++ (packing) and HLSL (unpacking).
#define GIGA_VOXEL_RT_HEADER_INDEX_SHIFT 16u
#define GIGA_VOXEL_RT_HEADER_INDEX_MASK  (0xFFu << GIGA_VOXEL_RT_HEADER_INDEX_SHIFT)
#define GIGA_VOXEL_RT_CHUNK_INDEX_MASK   0xFFFFu

#ifndef __cplusplus
// Decode a GigaVoxel RT InstanceCustomIndex into (header_idx, chunk_header_idx).
// Shader-only: HLSL `out` parameters have no C++ equivalent.
void DecodeGigaVoxelRTInstanceCustomIndex(uint IC, out uint HeaderIdx, out uint ChunkIdx) {
    HeaderIdx = (IC >> GIGA_VOXEL_RT_HEADER_INDEX_SHIFT) & 0xFFu;
    ChunkIdx  = IC & GIGA_VOXEL_RT_CHUNK_INDEX_MASK;
}
#endif // __cplusplus

MI_SHARED_HLSL_END

#endif
