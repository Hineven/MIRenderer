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
class ChunkData : public mi::RefCounted<> {
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

    // Get the full block identity {id, state} at the position (resolves palette).
    BlockData GetBlock(uint32_t lx, uint32_t ly, uint32_t lz) const;
    // Convenience: just the id.
    MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockId(uint32_t lx, uint32_t ly, uint32_t lz) const {
        return GetBlock(lx, ly, lz).id;
    }

    // Set the block at the specified position (auto-updates palette if needed).
    // BlockData overload carries state; BlockId overload defaults state to 0.
    void SetBlock(uint32_t lx, uint32_t ly, uint32_t lz, BlockData block);
    void SetBlock(uint32_t lx, uint32_t ly, uint32_t lz,
                  MACROMC_REGISTRY_NAMESPACE::BlockId block_id) {
        SetBlock(lx, ly, lz, BlockData{block_id, 0});
    }

    // Fill the entire chunk with a single block identity.
    void Fill(BlockData block);
    void Fill(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) {
        Fill(BlockData{block_id, 0});
    }

    // --- SubChunk access ---
    SubChunk& GetSubChunk(uint8_t subchunk_y);
    const SubChunk& GetSubChunk(uint8_t subchunk_y) const;

    // --- Palette access ---
    Palette& GetPalette() { return palette_; }
    const Palette& GetPalette() const { return palette_; }

    // Note: presence / sim state (ChunkPresence / ChunkSim) is NOT stored here.
    // ChunkData is a pure data container. Lifecycle + participation state lives
    // in ChunkRegistry's per-entry record, so a chunk referenced by multiple
    // shells never has ambiguous state. See chunk_registry.h.

    // --- Statistics ---

    // Count the number of non-empty SubChunks.
    size_t CountNonEmptySubChunks() const;

private:
    ChunkCoord coord_;
    Palette palette_;
    SubChunk sub_chunks_[kSubChunksPerChunkY];  // 256 subchunks

    // Initialize subchunk section_y values.
    void InitSubChunks();
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_CHUNK_DATA_H
