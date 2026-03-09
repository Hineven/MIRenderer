/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_TYPES_H
#define MACROMC_WORLD_TYPES_H

#include "world/common.h"
#include <glm/glm.hpp>
#include <cstdint>

MACROMC_WORLD_NAMESPACE_BEGIN

// Coordinate types
using BlockCoord = glm::ivec3;      // Block integer coordinates within Shell
using ChunkCoord = glm::ivec3;      // Chunk integer coordinates within World
using WorldPos = glm::vec3;         // World absolute floating coordinates

// Shell ID type
using ShellId = uint32_t;
static constexpr ShellId kInvalidShellId = 0;

// Chunk dimensions
static constexpr size_t kChunkSizeX = 16;
static constexpr size_t kChunkSizeY = 256;  // Height
static constexpr size_t kChunkSizeZ = 16;
static constexpr size_t kBlocksPerChunk = kChunkSizeX * kChunkSizeY * kChunkSizeZ;

// SubChunk dimensions (for LOD and Mesh building)
static constexpr size_t kSubChunkSize = 16;
static constexpr size_t kSubChunksPerChunkY = kChunkSizeY / kSubChunkSize;

// Block state max size
static constexpr size_t kBlockStateSize = 64;

// Helper function: Calculate block index in chunk
FORCEINLINE size_t GetBlockIndex(uint32_t x, uint32_t y, uint32_t z) {
    return static_cast<size_t>(y) * kChunkSizeZ * kChunkSizeX + 
           static_cast<size_t>(z) * kChunkSizeX + 
           static_cast<size_t>(x);
}

// Chunk state machine
enum class ChunkState : uint8_t {
    kUnloaded,          // Not loaded, exists only on disk/generator reference
    kGenerating,        // Generating terrain data
    kGenerated,         // Generation complete, waiting to load
    kLoading,           // Loading from disk/network
    kLoaded,            // Loaded to memory, ready to build mesh
    kMeshBuilding,      // Building VoxelMesh
    kReady,             // Fully ready, can render/interact
    kUnloading,         // Unloading (saving to disk/releasing memory)
    kMax
};

// Shell category (for Worldgen selection)
enum class ShellCategory : uint8_t {
    kTerrain,           // Terrain (main world)
    kStructure,         // Building structure
    kEmpty,             // Empty (player-built spaceship, etc.)
    kMax
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_TYPES_H
