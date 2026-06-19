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
// GigaVoxelChunkHeader (per-chunk, pure geometry addressing).
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
// Atlas is intentionally NOT here: the block atlas is process-global (one
// MC-resource-pack), carried per-asset in GigaVoxelHeader.AtlasBindlessIndex.
// =============================================================================
struct GigaVoxelChunkHeader {
    uint VertexOffset;        // Offset into the vertex uber buffer (in elements)
    uint IndexOffset;         // Offset into the index  uber buffer (in elements)
    uint VertexCount;
    uint IndexCount;
    uint _Padding0;
    uint _Padding1;
};

// =============================================================================
// GigaVoxelVertex: GPU-friendly vertex format for VC (Vanilla Chunk) geometry.
//
// Mirrors macromc::meshing::VoxelVertex field-for-field but lives in the
// renderer layer (no dependency on macromc) and is padded to a clean 48 bytes.
// The macromc host code converts VoxelVertex -> GigaVoxelVertex at upload time.
//
// Positions are already world-space (the greedy mesher emits world coords),
// so no per-chunk transform is needed at the vertex level.
//
// Atlas UV convention (see macromc meshing/types.h):
//   finalUv = uv_base + frac(localUv * uv_scale) * (1 / 256)
// keeps sampling inside one 16x16 tile (256x256 tile grid in a 4096^2 atlas).
// =============================================================================
struct GigaVoxelVertex {
    float3 position;        // world-space vertex position (meters)
    float3 normal;          // axis-aligned face normal (one of +-X/+-Y/+-Z)
    float2 uv_base;         // atlas-space origin (bottom-left) of the block's tile
    float2 uv_scale;        // how many tiles the owning quad spans (u, v)
    uint   texture_index;   // block tile index (== BlockDefinition::texture_index)
    uint   _pad;            // explicit pad -> 48 bytes, 4-byte aligned
};

MI_SHARED_HLSL_END

#endif
