/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_SUB_CHUNK_H
#define MACROMC_WORLD_SUB_CHUNK_H

#include "world/common.h"
#include "world/types.h"
#include "registry/types.h"
#include <memory>
#include <cstdint>

MACROMC_WORLD_NAMESPACE_BEGIN

// SubChunk: A 16x16x16 block section within a Chunk.
// This is the fundamental memory organization unit for voxel data.
//
// Storage optimization:
//   - kEmpty:   No memory allocated (all air). ~90% of subchunks in 4096m world.
//   - kUniform: Single palette index stored (all same block type). No array allocated.
//   - kMixed:   Full 4096-byte index array allocated.
//
// Note: This class stores palette INDICES, not BlockIds.
// To resolve actual BlockIds, use ChunkData which pairs SubChunk with Palette.
class SubChunk {
public:
    SubChunk();
    explicit SubChunk(uint8_t section_y);

    // Non-copyable, movable
    SubChunk(const SubChunk&) = delete;
    SubChunk& operator=(const SubChunk&) = delete;
    SubChunk(SubChunk&& other) noexcept;
    SubChunk& operator=(SubChunk&& other) noexcept;

    // --- Accessors ---

    // Get the palette index at (lx, sub_ly, lz). All coordinates are 0~15.
    uint8_t GetIndex(uint32_t lx, uint32_t sub_ly, uint32_t lz) const;

    // Set the palette index at (lx, sub_ly, lz).
    // Automatically upgrades state: kEmpty -> kUniform or kMixed, kUniform -> kMixed if needed.
    void SetIndex(uint32_t lx, uint32_t sub_ly, uint32_t lz, uint8_t palette_index);

    // Fill all 4096 positions with a single palette index.
    // Optimizes state: air index -> kEmpty, otherwise -> kUniform.
    void Fill(uint8_t palette_index);

    // --- State queries ---

    SubChunkState GetState() const { return state_; }
    bool IsEmpty() const { return state_ == SubChunkState::kEmpty; }
    bool IsUniform() const { return state_ == SubChunkState::kUniform; }
    bool IsMixed() const { return state_ == SubChunkState::kMixed; }

    uint8_t GetSectionY() const { return section_y_; }
    void SetSectionY(uint8_t y) { section_y_ = y; }

    // Get the uniform palette index (only valid when IsUniform()).
    uint8_t GetUniformIndex() const { return uniform_index_; }

    // Direct access to the index array (only valid when IsMixed()).
    const uint8_t* GetIndices() const { return indices_.get(); }
    uint8_t* GetMutableIndices() { return indices_.get(); }

    // --- State management ---

    // Release all data and reset to kEmpty state.
    void Release();

    // Allocate the 4096-byte index array (transitions to kMixed).
    void Allocate();

    // Recompute state from actual index data (scans the array).
    // Call this after external modifications to the index array.
    void RecomputeState();

private:
    uint8_t section_y_ = 0;
    SubChunkState state_ = SubChunkState::kEmpty;
    uint8_t uniform_index_ = 0;  // Valid when state_ == kUniform
    std::unique_ptr<uint8_t[]> indices_;  // Valid when state_ == kMixed (4096 bytes)

    static size_t GetLocalIndex(uint32_t lx, uint32_t sub_ly, uint32_t lz) {
        return GetBlockIndexInSubChunk(lx, sub_ly, lz);
    }
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_SUB_CHUNK_H
