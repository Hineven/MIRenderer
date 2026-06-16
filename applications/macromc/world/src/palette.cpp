/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/palette.h"

MACROMC_WORLD_NAMESPACE_BEGIN

Palette::Palette() {
    // Reserve slot 0 for Air (state 0). This invariant is REQUIRED by SubChunk's
    // state machine, which assumes palette index 0 == air everywhere
    // (kEmpty returns 0, SetIndex(0) is a no-op, Fill(0) -> kEmpty, etc.).
    entries_[0] = PaletteEntry{MACROMC_REGISTRY_NAMESPACE::kAirBlockId, 0};
    size_ = 1;
}

uint8_t Palette::AddBlock(const PaletteEntry& entry) {
    if (size_ >= kMaxSize) {
        return kInvalidIndex;
    }
    uint8_t index = static_cast<uint8_t>(size_++);
    entries_[index] = entry;
    return index;
}

BlockData Palette::GetBlockData(uint8_t index) const {
    if (index < size_) {
        return BlockData{entries_[index].id, entries_[index].state};
    }
    return BlockData{MACROMC_REGISTRY_NAMESPACE::kAirBlockId, 0};
}

uint8_t Palette::FindOrAdd(const PaletteEntry& entry) {
    auto found = Find(entry);
    if (found.has_value()) {
        return found.value();
    }
    return AddBlock(entry);
}

std::optional<uint8_t> Palette::Find(const PaletteEntry& entry) const {
    for (uint16_t i = 0; i < size_; ++i) {
        if (entries_[i] == entry) {
            return static_cast<uint8_t>(i);
        }
    }
    return std::nullopt;
}

void Palette::Clear() {
    // Preserve the air-at-slot-0 invariant (see constructor).
    entries_[0] = PaletteEntry{MACROMC_REGISTRY_NAMESPACE::kAirBlockId, 0};
    size_ = 1;
}

MACROMC_WORLD_NAMESPACE_END
