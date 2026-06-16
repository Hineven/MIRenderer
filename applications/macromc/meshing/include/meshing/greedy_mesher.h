/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_MESHING_GREEDY_MESHER_H
#define MACROMC_MESHING_GREEDY_MESHER_H

#include <cstdint>

#include "meshing/common.h"
#include "meshing/types.h"
#include "core/refcounted.h"
#include "registry/block_def.h"     // BlockDefinition
#include "registry/block_registry.h"// BlockRegistry
#include "world/chunk_data.h"       // ChunkData
#include "world/chunk_coord.h"
#include "world/types.h"

#include <glm/glm.hpp>

MACROMC_MESHING_NAMESPACE_BEGIN

// A read-only voxel accessor that can cross chunk boundaries.
// The greedy mesher queries block ids via this interface so that boundary
// faces (which touch neighbor chunks / neighbor subchunks) can be culled
// correctly. Returning kAirBlockId ("air") for out-of-range or unloaded
// positions yields a conservative mesh: boundary faces are kept when the
// neighbor is unknown, matching the streaming design in PLAN.md §3.4.
//
// Implementations:
//   - SingleChunkVoxelSource: only the center chunk is known; everything
//     outside is treated as air (conservative boundary faces).
//   - NeighborAwareVoxelSource (in ChunkMeshingContext): resolves the 6
//     neighbor chunks under a shared lock.
class VoxelSource {
public:
    virtual ~VoxelSource() = default;

    // Get the BlockId at world-space block coordinates (bx, by, bz).
    // Must be safe to call from worker threads. Out-of-loaded-range queries
    // return kAirBlockId (air) -> conservative meshing.
    virtual MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockIdAtWorld(int32_t bx, int32_t by, int32_t bz) const = 0;
};

// Conservative single-chunk source: knows only one chunk, treats everything
// outside it as air. Useful for unit-testing the algorithm in isolation and
// as the fallback when neighbors are not loaded.
class SingleChunkVoxelSource : public VoxelSource {
public:
    // chunk_origin = world-space block origin (0,0,0 corner) of the chunk.
    explicit SingleChunkVoxelSource(const MACROMC_WORLD_NAMESPACE::ChunkData& chunk,
                                    const MACROMC_WORLD_NAMESPACE::ChunkCoord& chunk_coord);

    MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockIdAtWorld(int32_t bx, int32_t by, int32_t bz) const override;

private:
    const MACROMC_WORLD_NAMESPACE::ChunkData& chunk_;
    MACROMC_WORLD_NAMESPACE::ChunkCoord chunk_coord_;
    glm::ivec3 origin_;   // world-space block origin of the chunk
};

// GreedyMesher: stateless greedy meshing over a 16x16x16 subchunk.
//
// Algorithm (per the design in gigavoxel_lod_definition.md §3.1 / §7.3):
//   - Iterate the 6 face directions independently.
//   - For each direction, build a 16x16 mask of "visible face" flags: a face
//     on block B is visible iff B is renderable AND the neighbor across that
//     face is not face-solid (air, water, glass, ...).
//   - Greedily merge contiguous same-block-type visible faces into quads.
//   - Each merged quad spanning (w,d) blocks emits 4 VoxelVertex carrying
//     uv_base (the block's atlas tile origin) + uv_scale (w,d) for frac()
//     tiling in the shader.
//
// The mesher is constructed with the BlockRegistry (to resolve per-face
// solid/render flags + texture indices) and reused across many subchunks.
class GreedyMesher : public mi::RefCounted<> {
public:
    explicit GreedyMesher(const MACROMC_REGISTRY_NAMESPACE::BlockRegistry& registry);

    // Mesh one subchunk of `chunk` into `out_mesh`.
    //   chunk_coord : world-space chunk coordinate of `chunk` (for neighbor lookup).
    //   subchunk_y  : subchunk index within the chunk (0..255).
    //   source      : voxel accessor for neighbor queries (crosses chunk boundary).
    //   block_origin: world-space block position (bx,by,bz) of the subchunk's
    //                 local (0,0,0) corner. Vertex positions are emitted in
    //                 this world space.
    void MeshSubChunk(const MACROMC_WORLD_NAMESPACE::ChunkData& chunk,
                      const MACROMC_WORLD_NAMESPACE::ChunkCoord& chunk_coord,
                      uint8_t subchunk_y,
                      const VoxelSource& source,
                      glm::ivec3 block_origin,
                      VoxelMesh& out_mesh) const;

private:
    const MACROMC_REGISTRY_NAMESPACE::BlockRegistry& registry_;

    // Resolve the atlas tile index for a block id's `face`. Air returns 0.
    uint32_t GetTextureIndex(MACROMC_REGISTRY_NAMESPACE::BlockId block_id,
                             MACROMC_WORLD_NAMESPACE::BlockFace face) const;

    // Is `block_id`'s `face` solid (culls the neighbor's opposing face)?
    bool IsFaceSolid(MACROMC_REGISTRY_NAMESPACE::BlockId block_id, MACROMC_WORLD_NAMESPACE::BlockFace face) const;

    // Should this block be rendered at all?
    bool IsRenderable(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) const;

    // Meshing primitive for one axis direction. See MeshSubChunk for param meaning.
    // d            : axis index 0=X, 1=Y, 2=Z (sweep along this axis).
    // u, v         : the two transverse axes.
    void MeshFaceDirection(const MACROMC_WORLD_NAMESPACE::ChunkData& chunk,
                           const VoxelSource& source,
                           glm::ivec3 block_origin,
                           int d, int u, int v,
                           int8_t normal_sign,   // +1 (positive face) or -1 (negative face)
                           VoxelMesh& out_mesh) const;
};

MACROMC_MESHING_NAMESPACE_END

#endif // MACROMC_MESHING_GREEDY_MESHER_H
