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
using WorldPos = glm::vec3;         // World absolute floating coordinates
using SubChunkCoord = glm::ivec3;   // SubChunk integer coordinates (world space)

// ChunkCoord: integer coordinates of a chunk in the XZ plane.
//
// A chunk spans the FULL world height (kChunkSizeY = 4096), so there is no
// per-chunk Y coordinate — a chunk is a vertical column. ChunkCoord is
// therefore a 2D {x, z} value. This is a dedicated struct (not glm::ivec2) so
// the field names (.x, .z) match their semantic axis, and so that accidentally
// indexing a .y component is a compile error rather than a silent dummy 0.
struct ChunkCoord {
    int x = 0;
    int z = 0;

    constexpr ChunkCoord() = default;
    constexpr ChunkCoord(int x_, int z_) : x(x_), z(z_) {}

    constexpr bool operator==(const ChunkCoord& other) const {
        return x == other.x && z == other.z;
    }
    constexpr bool operator!=(const ChunkCoord& other) const {
        return !(*this == other);
    }
};

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

// Chunk presence — voxel data lifecycle (ownership of the authoritative voxel data).
// Orthogonal axis #1. Owned and driven by ChunkRegistry (S0 stage); ChunkData
// itself is a pure data container and holds no presence state.
enum class ChunkPresence : uint8_t {
    kAbsent,        // Not in the registry (placeholder; registry usually omits these).
    kLoading,       // Asynchronous generation / load in flight.
    kReady,         // Voxel data resident and stable; readable by S2 derivation.
    kUnloading,     // Being unloaded (saved / released); entry torn down this tick.
};

// Chunk sim state — gameplay participation (whether the chunk ticks in S1).
// Orthogonal axis #2. Owned by ChunkRegistry; driven by player movement across
// chunk borders (active-set / borderline-ring updates in S0).
enum class ChunkSim : uint8_t {
    kInactive,      // Resident but outside the simulation envelope; does not tick.
    kBorderline,    // Weak-load neighbour ring: kReady but does not tick; exists only
                    //   so active-edge chunks can read neighbours (15-block rule).
    kActive,        // Participates in this tick's S1 checkerboard simulation.
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
