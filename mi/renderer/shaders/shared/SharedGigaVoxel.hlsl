// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_GIGA_VOXEL_HLSL
#define MI_RENDERER_SHADERS_SHARED_GIGA_VOXEL_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// =============================================================================
// GigaVoxel device-side header.
//
// One GigaVoxel asset occupies a single bindless slot in the
// DeviceBindlessResourceAllocator; this header (stored in the
// GigaVoxelHeaderBuffer, indexed by the slot) points the shader at the
// GigaVoxel's slice of the global vertex/index uber buffers and its
// block-texture atlas bindless index.
//
// Offsets follow the same convention as StaticMeshHeader / GeometryHeader:
//   - VertexOffset is measured in elements (sizeof(GigaVoxelVertex) units).
//   - IndexOffset  is measured in elements (sizeof(uint32) units).
//
// NOTE: This is the Phase-1 skeleton layout. A single GigaVoxel currently
// merges all uploaded chunk geometry into one vertex/index range + one BLAS.
// Per-chunk BLAS / streaming / culling will extend this in later phases.
// =============================================================================
struct GigaVoxelHeader {
    uint VertexOffset;        // Offset into the vertex uber buffer (in elements)
    uint IndexOffset;         // Offset into the index  uber buffer (in elements)
    uint VertexCount;
    uint IndexCount;
    uint AtlasBindlessIndex;  // Bindless descriptor index of the 4096^2 block atlas (SRV)
    uint3 _Padding;
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
