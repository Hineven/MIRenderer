/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_MESHING_TYPES_H
#define MACROMC_MESHING_TYPES_H

#include <cstdint>
#include <vector>

#include "meshing/common.h"
#include "registry/types.h"   // BlockId
#include "world/types.h"      // BlockFace, kSubChunkSize, kSubChunksPerChunkY

#include <glm/glm.hpp>

MACROMC_MESHING_NAMESPACE_BEGIN

// =============================================================================
// Block texture atlas model (CPU side).
//
// Design: ONE global 4096x4096 2D texture. Each block's tile is 16x16 texels,
// laid out in a 256x256 grid => 65536 tiles == kMaxBlockTypes.
//
// UV convention: a block with texture_index t occupies the tile at grid cell
//   tileX = t % 256, tileY = t / 256
// whose atlas-space origin (bottom-left of the tile, in 0..1 atlas UV) is
//   uvBase = (tileX / 256, tileY / 256)
// and the tile's UV extent (one tile) is 1/256 in both axes.
//
// A greedy quad spanning N blocks of the same type repeats its tile N times via
// frac(): the vertex stores uv_base + uv_scale(=N tiles), and the shader does
//   finalUv = uv_base + frac(localUv * uv_scale) * (1/256)
// which keeps sampling inside the tile => no cross-tile bleed with a clamp sampler.
//
// This CPU-side module only computes uv_base and uv_scale (integer tile counts
// encoded as floats); atlas GPU upload is a separate task.
// =============================================================================

// Atlas layout constants.
static constexpr uint32_t kAtlasTileTexelSize = 16;          // 16x16 texels per block tile
static constexpr uint32_t kAtlasTilesPerRow   = 256;         // 4096 / 16
static constexpr uint32_t kAtlasTilesPerCol   = 256;         // 4096 / 16
static constexpr uint32_t kAtlasTileCount     = kAtlasTilesPerRow * kAtlasTilesPerCol; // 65536 == kMaxBlockTypes

// Reciprocal of tiles-per-row, in float (== tile UV extent per axis).
static constexpr float kAtlasTileUVExtent = 1.0f / static_cast<float>(kAtlasTilesPerRow);

// Compute the atlas-space origin (bottom-left) of a tile by its index.
// Returns UV in 0..1 atlas space.
FORCEINLINE glm::vec2 AtlasTileOriginUV(MACROMC_REGISTRY_NAMESPACE::BlockId tile_index) {
    uint32_t t = tile_index;
    uint32_t tileX = t % kAtlasTilesPerRow;
    uint32_t tileY = t / kAtlasTilesPerRow;
    return glm::vec2(tileX * kAtlasTileUVExtent, tileY * kAtlasTileUVExtent);
}

// =============================================================================
// VoxelVertex: greedy-meshed vertex format.
//
// Carries uv_base + uv_scale (+ texture_index) so the fragment shader can
// reconstruct the per-pixel atlas UV with frac() tiling. Decoupled from the
// renderer's DefaultStaticMeshVertex; uploaded via a dedicated geometry heap
// in a future task.
//
// Extensibility: PBR channels (normal/ORM/emissive maps) and per-face tiles
// can be added here without changing the greedy-meshing algorithm.
// =============================================================================

struct VoxelVertex {
    glm::vec3 position;        // world-space vertex position (meters)
    glm::vec3 normal;          // face normal (one of ±X/±Y/±Z)
    glm::vec2 uv_base;         // atlas-space origin (bottom-left) of the block's tile
    glm::vec2 uv_scale;        // how many tiles the owning quad spans (u, v), integer counts as float
    uint32_t  texture_index;   // block tile index (== BlockDefinition::texture_index), for future use / debug

    VoxelVertex() = default;
    VoxelVertex(glm::vec3 pos, glm::vec3 nrm, glm::vec2 base, glm::vec2 scale, uint32_t tex)
        : position(pos), normal(nrm), uv_base(base), uv_scale(scale), texture_index(tex) {}
};

// VoxelMesh: output of greedy meshing for one meshing unit (per subchunk by default).
// Pure host data; handed to a render-thread upload path in a later task.
struct VoxelMesh {
    std::vector<VoxelVertex>  vertices;
    std::vector<uint32_t>     indices;

    void Clear() {
        vertices.clear();
        indices.clear();
    }

    bool IsEmpty() const { return indices.empty(); }

    uint32_t GetTriangleCount() const {
        return static_cast<uint32_t>(indices.size()) / 3;
    }
};

// Result of meshing a whole chunk: one VoxelMesh per subchunk (0..kSubChunksPerChunkY-1).
// Null entry means the subchunk produced no geometry (was empty / all culled).
struct VoxelMeshJobResult {
    std::vector<VoxelMesh> subchunk_meshes;   // size == kSubChunksPerChunkY

    void Init() {
        subchunk_meshes.resize(MACROMC_WORLD_NAMESPACE::kSubChunksPerChunkY);
    }

    void Clear() {
        for (auto& m : subchunk_meshes) m.Clear();
    }

    uint32_t GetTotalTriangleCount() const {
        uint32_t total = 0;
        for (const auto& m : subchunk_meshes) total += m.GetTriangleCount();
        return total;
    }
};

MACROMC_MESHING_NAMESPACE_END

#endif // MACROMC_MESHING_TYPES_H
