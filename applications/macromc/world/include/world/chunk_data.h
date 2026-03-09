/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_CHUNK_DATA_H
#define MACROMC_WORLD_CHUNK_DATA_H

#include "world/common.h"
#include "world/types.h"
#include "world/block_data.h"
#include "core/refcounted.h"
#include <memory>

MACROMC_WORLD_NAMESPACE_BEGIN

// ChunkData: Pure data container for a chunk
class ChunkData : public mi::RefCounted {
public:
    explicit ChunkData(ChunkCoord coord);
    ~ChunkData() override;
    
    // Non-copyable
    ChunkData(const ChunkData&) = delete;
    ChunkData& operator=(const ChunkData&) = delete;
    
    // Coordinate access
    ChunkCoord GetCoord() const { return coord_; }
    
    // Block access (local coordinates within chunk)
    BlockData& GetBlock(uint32_t x, uint32_t y, uint32_t z);
    const BlockData& GetBlock(uint32_t x, uint32_t y, uint32_t z) const;
    void SetBlock(uint32_t x, uint32_t y, uint32_t z, const BlockData& block);
    
    // State management
    ChunkState GetState() const { return state_; }
    void SetState(ChunkState state) { state_ = state; }
    
    // Quick fill (for Worldgen)
    void Fill(const BlockData& block);
    
    // Check if blocks are allocated
    bool IsAllocated() const { return blocks_ != nullptr; }
    void AllocateBlocks();
    
private:
    ChunkCoord coord_;
    ChunkState state_ = ChunkState::kUnloaded;
    std::unique_ptr<BlockData[]> blocks_;  // 16*256*16 = 65536 blocks
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_CHUNK_DATA_H
