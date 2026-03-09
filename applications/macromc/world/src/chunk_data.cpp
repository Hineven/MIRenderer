/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/chunk_data.h"

MACROMC_WORLD_NAMESPACE_BEGIN

ChunkData::ChunkData(ChunkCoord coord) 
    : coord_(coord) {
}

ChunkData::~ChunkData() = default;

void ChunkData::AllocateBlocks() {
    if (!blocks_) {
        blocks_ = std::make_unique<BlockData[]>(kBlocksPerChunk);
    }
}

BlockData& ChunkData::GetBlock(uint32_t x, uint32_t y, uint32_t z) {
    if (!blocks_) {
        AllocateBlocks();
    }
    return blocks_[GetBlockIndex(x, y, z)];
}

const BlockData& ChunkData::GetBlock(uint32_t x, uint32_t y, uint32_t z) const {
    // Note: This will crash if blocks_ is null
    // Caller should ensure blocks are allocated
    return blocks_[GetBlockIndex(x, y, z)];
}

void ChunkData::SetBlock(uint32_t x, uint32_t y, uint32_t z, const BlockData& block) {
    if (!blocks_) {
        AllocateBlocks();
    }
    blocks_[GetBlockIndex(x, y, z)] = block;
}

void ChunkData::Fill(const BlockData& block) {
    if (!blocks_) {
        AllocateBlocks();
    }
    for (size_t i = 0; i < kBlocksPerChunk; ++i) {
        blocks_[i] = block;
    }
}

MACROMC_WORLD_NAMESPACE_END
