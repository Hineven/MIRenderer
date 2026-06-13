/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/palette.h"

MACROMC_WORLD_NAMESPACE_BEGIN

Palette::Palette() {
    for (auto& entry : entries_) {
        entry = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
    }
}

uint8_t Palette::AddBlock(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) {
    if (size_ >= kMaxSize) {
        return kInvalidIndex;
    }
    uint8_t index = static_cast<uint8_t>(size_++);
    entries_[index] = block_id;
    return index;
}

MACROMC_REGISTRY_NAMESPACE::BlockId Palette::GetBlockId(uint8_t index) const {
    if (index < size_) {
        return entries_[index];
    }
    return MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
}

uint8_t Palette::FindOrAdd(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) {
    auto found = Find(block_id);
    if (found.has_value()) {
        return found.value();
    }
    return AddBlock(block_id);
}

std::optional<uint8_t> Palette::Find(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) const {
    for (uint16_t i = 0; i < size_; ++i) {
        if (entries_[i] == block_id) {
            return static_cast<uint8_t>(i);
        }
    }
    return std::nullopt;
}

void Palette::Clear() {
    size_ = 0;
}

MACROMC_WORLD_NAMESPACE_END
