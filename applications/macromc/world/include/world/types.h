/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_TYPES_H
#define MACROMC_WORLD_TYPES_H

#include "world/common.h"
#include "registry/types.h"  // BlockFace, BlockId, ... (face enum lives in registry)
#include <glm/glm.hpp>
#include <cstdint>

MACROMC_WORLD_NAMESPACE_BEGIN

// Coordinate types
using BlockCoord = glm::ivec3;      // Block integer coordinates (world space)
using ChunkCoord = glm::ivec3;      // Chunk integer coordinates within World
using WorldPos = glm::vec3;         // World absolute floating coordinates
using SubChunkCoord = glm::ivec3;   // SubChunk integer coordinates (world space)

// Shell ID type
using ShellId = uint32_t;
static constexpr ShellId kInvalidShellId = 0;

// Chunk dimensions
static constexpr size_t kChunkSizeX = 16;
static constexpr size_t kChunkSizeY = 4096;  // World height
static constexpr size_t kChunkSizeZ = 16;
static constexpr size_t kBlocksPerChunk = kChunkSizeX * kChunkSizeY * kChunkSizeZ;  // 1,048,576

// SubChunk dimensions (16x16x16, MC classic)
static constexpr size_t kSubChunkSize = 16;
static constexpr size_t kSubChunksPerChunkY = kChunkSizeY / kSubChunkSize;  // 256
static constexpr size_t kBlocksPerSubChunk = kSubChunkSize * kSubChunkSize * kSubChunkSize;  // 4096

// Palette constants
static constexpr size_t kMaxPaletteSize = 256;

// Block state max size (for future use, not stored in core path)
static constexpr size_t kBlockStateSize = 64;

// Note: BlockFace is defined in registry/types.h and shared across modules.

// Helper function: Calculate block index within a SubChunk (0~4095)
FORCEINLINE size_t GetBlockIndexInSubChunk(uint32_t lx, uint32_t sub_ly, uint32_t lz) {
    return static_cast<size_t>(sub_ly) * kSubChunkSize * kSubChunkSize +
           static_cast<size_t>(lz) * kSubChunkSize +
           static_cast<size_t>(lx);
}

// SubChunk state (compression-friendly classification)
enum class SubChunkState : uint8_t {
    kEmpty,     // All blocks are air — no memory allocated
    kUniform,   // All blocks are the same type — single palette index stored
    kMixed      // Multiple block types — full 4096-byte index array allocated
};

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
