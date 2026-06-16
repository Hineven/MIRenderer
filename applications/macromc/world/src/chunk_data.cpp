/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/chunk_data.h"
#include "world/chunk_coord.h"

MACROMC_WORLD_NAMESPACE_BEGIN

ChunkData::ChunkData(ChunkCoord coord)
    : coord_(coord) {
    InitSubChunks();
}

ChunkData::~ChunkData() = default;

void ChunkData::InitSubChunks() {
    for (size_t i = 0; i < kSubChunksPerChunkY; ++i) {
        sub_chunks_[i].SetSectionY(static_cast<uint8_t>(i));
    }
}

BlockData ChunkData::GetBlock(uint32_t lx, uint32_t ly, uint32_t lz) const {
    uint8_t sub_y = LocalYToSubChunkIndex(ly);
    uint32_t sub_ly = ly - SubChunkIndexToLocalY(sub_y);

    uint8_t palette_index = sub_chunks_[sub_y].GetIndex(lx, sub_ly, lz);
    return palette_.GetBlockData(palette_index);
}

void ChunkData::SetBlock(uint32_t lx, uint32_t ly, uint32_t lz, BlockData block) {
    uint8_t sub_y = LocalYToSubChunkIndex(ly);
    uint32_t sub_ly = ly - SubChunkIndexToLocalY(sub_y);

    // Ensure {id, state} is in the palette (add if needed)
    uint8_t palette_index = palette_.FindOrAdd(block);

    // Set the palette index in the subchunk
    sub_chunks_[sub_y].SetIndex(lx, sub_ly, lz, palette_index);
}

void ChunkData::Fill(BlockData block) {
    uint8_t palette_index = palette_.FindOrAdd(block);

    for (size_t i = 0; i < kSubChunksPerChunkY; ++i) {
        sub_chunks_[i].Fill(palette_index);
    }
}

SubChunk& ChunkData::GetSubChunk(uint8_t subchunk_y) {
    return sub_chunks_[subchunk_y];
}

const SubChunk& ChunkData::GetSubChunk(uint8_t subchunk_y) const {
    return sub_chunks_[subchunk_y];
}

size_t ChunkData::CountNonEmptySubChunks() const {
    size_t count = 0;
    for (size_t i = 0; i < kSubChunksPerChunkY; ++i) {
        if (!sub_chunks_[i].IsEmpty()) {
            ++count;
        }
    }
    return count;
}

MACROMC_WORLD_NAMESPACE_END
