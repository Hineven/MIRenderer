/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_CHUNK_DATA_H
#define MACROMC_WORLD_CHUNK_DATA_H

#include "world/common.h"
#include "world/types.h"
#include "world/palette.h"
#include "world/sub_chunk.h"
#include "core/refcounted.h"

MACROMC_WORLD_NAMESPACE_BEGIN

// ChunkData: Pure data container for a chunk.
//
// Architecture:
//   Chunk = Palette + SubChunk[256]
//   - Palette maps compact 8-bit indices to actual BlockIds (shared across all SubChunks)
//   - Each SubChunk is 16x16x16 blocks, storing palette indices
//   - Empty SubChunks consume zero memory (~90% of a 4096m-height chunk)
//   - Uniform SubChunks store a single index (no array allocated)
//
// Chunk dimensions: 16 x 4096 x 16 = 1,048,576 blocks
class ChunkData : public mi::RefCounted {
public:
    explicit ChunkData(ChunkCoord coord);
    ~ChunkData() override;

    // Non-copyable
    ChunkData(const ChunkData&) = delete;
    ChunkData& operator=(const ChunkData&) = delete;

    // --- Coordinate access ---
    ChunkCoord GetCoord() const { return coord_; }

    // --- Block access (local coordinates within chunk) ---
    // lx: 0~15, ly: 0~4095, lz: 0~15

    // Get the BlockId at the specified position (resolves through palette).
    MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockId(uint32_t lx, uint32_t ly, uint32_t lz) const;

    // Set the block at the specified position (auto-updates palette if needed).
    void SetBlock(uint32_t lx, uint32_t ly, uint32_t lz,
                  MACROMC_REGISTRY_NAMESPACE::BlockId block_id);

    // Fill the entire chunk with a single block type.
    void Fill(MACROMC_REGISTRY_NAMESPACE::BlockId block_id);

    // --- SubChunk access ---
    SubChunk& GetSubChunk(uint8_t subchunk_y);
    const SubChunk& GetSubChunk(uint8_t subchunk_y) const;

    // --- Palette access ---
    Palette& GetPalette() { return palette_; }
    const Palette& GetPalette() const { return palette_; }

    // --- State management ---
    ChunkState GetState() const { return state_; }
    void SetState(ChunkState state) { state_ = state; }

    // --- Statistics ---

    // Count the number of non-empty SubChunks.
    size_t CountNonEmptySubChunks() const;

private:
    ChunkCoord coord_;
    ChunkState state_ = ChunkState::kUnloaded;
    Palette palette_;
    SubChunk sub_chunks_[kSubChunksPerChunkY];  // 256 subchunks

    // Initialize subchunk section_y values.
    void InitSubChunks();
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_CHUNK_DATA_H
