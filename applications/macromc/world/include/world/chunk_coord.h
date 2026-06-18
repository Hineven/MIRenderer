/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_CHUNK_COORD_H
#define MACROMC_WORLD_CHUNK_COORD_H

#include "world/common.h"
#include "world/types.h"
#include <glm/glm.hpp>

MACROMC_WORLD_NAMESPACE_BEGIN

// =============================================================================
// Coordinate conversion utilities
// Centralizes all block <-> chunk <-> subchunk coordinate transformations.
// =============================================================================

// Convert a world-space block position to the ChunkCoord that contains it.
// Uses floor-division to handle negative coordinates correctly.
// Result is 2D (XZ) — a chunk spans the full world height, so block Y maps to
// a local Y inside the chunk, not to a chunk coordinate.
FORCEINLINE ChunkCoord BlockWorldToChunkCoord(const glm::ivec3& block_world_pos) {
    return ChunkCoord(
        (block_world_pos.x >= 0) ? (block_world_pos.x / static_cast<int>(kChunkSizeX))
                                 : ((block_world_pos.x + 1) / static_cast<int>(kChunkSizeX) - 1),
        (block_world_pos.z >= 0) ? (block_world_pos.z / static_cast<int>(kChunkSizeZ))
                                 : ((block_world_pos.z + 1) / static_cast<int>(kChunkSizeZ) - 1)
    );
}

// Convert a world-space block position to local coordinates within its chunk.
// Result: lx in [0, kChunkSizeX), ly in [0, kChunkSizeY), lz in [0, kChunkSizeZ)
FORCEINLINE glm::ivec3 BlockWorldToLocal(const glm::ivec3& block_world_pos) {
    ChunkCoord cc = BlockWorldToChunkCoord(block_world_pos);
    return glm::ivec3(
        block_world_pos.x - cc.x * static_cast<int>(kChunkSizeX),
        block_world_pos.y,
        block_world_pos.z - cc.z * static_cast<int>(kChunkSizeZ)
    );
}

// Convert a ChunkCoord to the world-space block position of its origin (0,0,0 corner).
// Y origin is 0 (chunk spans full height from the world bottom).
FORCEINLINE glm::ivec3 ChunkCoordToBlockOrigin(const ChunkCoord& chunk_coord) {
    return glm::ivec3(
        chunk_coord.x * static_cast<int>(kChunkSizeX),
        0,
        chunk_coord.z * static_cast<int>(kChunkSizeZ)
    );
}

// Convert a local Y coordinate (0~4095) to the SubChunk index (0~255).
FORCEINLINE uint8_t LocalYToSubChunkIndex(uint32_t ly) {
    return static_cast<uint8_t>(ly / kSubChunkSize);
}

// Convert a SubChunk index (0~255) to the local Y start of that SubChunk.
FORCEINLINE uint32_t SubChunkIndexToLocalY(uint8_t subchunk_y) {
    return static_cast<uint32_t>(subchunk_y) * kSubChunkSize;
}

// Convert a world-space block position to SubChunk world-space coordinates.
FORCEINLINE SubChunkCoord BlockWorldToSubChunkCoord(const glm::ivec3& block_world_pos) {
    return SubChunkCoord(
        (block_world_pos.x >= 0) ? (block_world_pos.x / static_cast<int>(kSubChunkSize))
                                 : ((block_world_pos.x + 1) / static_cast<int>(kSubChunkSize) - 1),
        (block_world_pos.y >= 0) ? (block_world_pos.y / static_cast<int>(kSubChunkSize))
                                 : ((block_world_pos.y + 1) / static_cast<int>(kSubChunkSize) - 1),
        (block_world_pos.z >= 0) ? (block_world_pos.z / static_cast<int>(kSubChunkSize))
                                 : ((block_world_pos.z + 1) / static_cast<int>(kSubChunkSize) - 1)
    );
}

// Get the opposite face (used for neighbor queries in meshing).
FORCEINLINE BlockFace OppositeFace(BlockFace face) {
    // Pairs: PosX<->NegX, PosY<->NegY, PosZ<->NegZ (even<->odd)
    return static_cast<BlockFace>(static_cast<uint8_t>(face) ^ 1);
}

// =============================================================================
// Region coordinates.
// A region is a kRegionSize x kRegionSize group of chunks in the XZ plane
// (chunks span full height, so regions are 2D). Regions are the checkerboard
// scheduling and event-routing work unit (see event_bus.h / TICK_PACING).
// =============================================================================
static constexpr int kRegionSize = 8;

// Convert a chunk coord to its containing region coord. Floor-division handles
// negatives correctly. The result is a ChunkCoord (2D XZ) at the region's
// chunk-grid origin.
FORCEINLINE ChunkCoord ChunkToRegionCoord(const ChunkCoord& cc) {
    return ChunkCoord(
        (cc.x >= 0) ? cc.x / kRegionSize : (cc.x + 1) / kRegionSize - 1,
        (cc.z >= 0) ? cc.z / kRegionSize : (cc.z + 1) / kRegionSize - 1
    );
}

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_CHUNK_COORD_H
