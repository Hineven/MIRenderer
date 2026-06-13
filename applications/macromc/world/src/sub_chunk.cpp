/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/sub_chunk.h"
#include <cstring>

MACROMC_WORLD_NAMESPACE_BEGIN

SubChunk::SubChunk() = default;

SubChunk::SubChunk(uint8_t section_y)
    : section_y_(section_y) {
}

SubChunk::SubChunk(SubChunk&& other) noexcept = default;

SubChunk& SubChunk::operator=(SubChunk&& other) noexcept = default;

uint8_t SubChunk::GetIndex(uint32_t lx, uint32_t sub_ly, uint32_t lz) const {
    switch (state_) {
    case SubChunkState::kEmpty:
        return 0;  // Air palette index
    case SubChunkState::kUniform:
        return uniform_index_;
    case SubChunkState::kMixed:
        return indices_[GetLocalIndex(lx, sub_ly, lz)];
    default:
        return 0;
    }
}

void SubChunk::SetIndex(uint32_t lx, uint32_t sub_ly, uint32_t lz, uint8_t palette_index) {
    size_t idx = GetLocalIndex(lx, sub_ly, lz);

    switch (state_) {
    case SubChunkState::kEmpty:
        if (palette_index == 0) {
            return;  // Already air
        }
        // Upgrade to mixed: allocate and set single block
        indices_ = std::make_unique<uint8_t[]>(kBlocksPerSubChunk);
        std::memset(indices_.get(), 0, kBlocksPerSubChunk);
        indices_[idx] = palette_index;
        state_ = SubChunkState::kMixed;
        break;

    case SubChunkState::kUniform:
        if (palette_index == uniform_index_) {
            return;  // Same value, no-op
        }
        // Upgrade to mixed: allocate, fill with uniform value, then set the target
        indices_ = std::make_unique<uint8_t[]>(kBlocksPerSubChunk);
        std::memset(indices_.get(), uniform_index_, kBlocksPerSubChunk);
        indices_[idx] = palette_index;
        state_ = SubChunkState::kMixed;
        break;

    case SubChunkState::kMixed:
        indices_[idx] = palette_index;
        break;
    }
}

void SubChunk::Fill(uint8_t palette_index) {
    if (palette_index == 0) {
        // All air -> empty, release memory
        Release();
        return;
    }

    // Uniform: store single index, release array if allocated
    uniform_index_ = palette_index;
    indices_.reset();
    state_ = SubChunkState::kUniform;
}

void SubChunk::Release() {
    indices_.reset();
    uniform_index_ = 0;
    state_ = SubChunkState::kEmpty;
}

void SubChunk::Allocate() {
    if (state_ == SubChunkState::kMixed) {
        return;  // Already allocated
    }

    indices_ = std::make_unique<uint8_t[]>(kBlocksPerSubChunk);

    if (state_ == SubChunkState::kUniform) {
        // Fill with uniform value
        std::memset(indices_.get(), uniform_index_, kBlocksPerSubChunk);
    } else {
        // Fill with air (0)
        std::memset(indices_.get(), 0, kBlocksPerSubChunk);
    }

    state_ = SubChunkState::kMixed;
}

void SubChunk::RecomputeState() {
    if (state_ != SubChunkState::kMixed || !indices_) {
        return;
    }

    // Check if all air
    bool all_air = true;
    for (size_t i = 0; i < kBlocksPerSubChunk; ++i) {
        if (indices_[i] != 0) {
            all_air = false;
            break;
        }
    }

    if (all_air) {
        Release();
        return;
    }

    // Check if all uniform
    uint8_t first = indices_[0];
    bool all_same = true;
    for (size_t i = 1; i < kBlocksPerSubChunk; ++i) {
        if (indices_[i] != first) {
            all_same = false;
            break;
        }
    }

    if (all_same) {
        uniform_index_ = first;
        indices_.reset();
        state_ = SubChunkState::kUniform;
    }
}

MACROMC_WORLD_NAMESPACE_END
