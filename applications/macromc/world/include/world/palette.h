/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_PALETTE_H
#define MACROMC_WORLD_PALETTE_H

#include "world/common.h"
#include "world/types.h"
#include "world/block_data.h"
#include "registry/types.h"
#include <cstdint>
#include <optional>

MACROMC_WORLD_NAMESPACE_BEGIN

// A palette entry: the full block identity = {id, state}.
// Two blocks with the same id but different state occupy distinct palette
// slots (e.g. top-slab vs bottom-slab). This is the dedup key for palette
// compression.
struct PaletteEntry {
    MACROMC_REGISTRY_NAMESPACE::BlockId id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
    uint16_t state = 0;

    bool operator==(const PaletteEntry&) const = default;
};

// Palette: Chunk-level block palette for palette compression.
// All SubChunks within a chunk share one Palette.
// Maps compact 8-bit indices (0~255) to palette entries {id, state}.
class Palette {
public:
    Palette();

    // Add a new entry to the palette. Returns the assigned index.
    // Returns kInvalidIndex if the palette is full (256 entries).
    // (BlockId overload defaults state to 0 — for blocks without state.)
    uint8_t AddBlock(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) {
        return AddBlock(PaletteEntry{block_id, 0});
    }
    uint8_t AddBlock(const PaletteEntry& entry);
    uint8_t AddBlock(const BlockData& block) {
        return AddBlock(PaletteEntry{block.id, block.state});
    }

    // Look up the full entry {id, state} for a given palette index.
    BlockData GetBlockData(uint8_t index) const;
    // Convenience: just the id.
    MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockId(uint8_t index) const {
        return GetBlockData(index).id;
    }

    // Find an existing entry or add a new one. Returns the index.
    // Returns kInvalidIndex if not found and palette is full.
    uint8_t FindOrAdd(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) {
        return FindOrAdd(PaletteEntry{block_id, 0});
    }
    uint8_t FindOrAdd(const PaletteEntry& entry);
    uint8_t FindOrAdd(const BlockData& block) {
        return FindOrAdd(PaletteEntry{block.id, block.state});
    }

    // Find an existing entry. Returns nullopt if not found.
    std::optional<uint8_t> Find(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) const {
        return Find(PaletteEntry{block_id, 0});
    }
    std::optional<uint8_t> Find(const PaletteEntry& entry) const;
    std::optional<uint8_t> Find(const BlockData& block) const {
        return Find(PaletteEntry{block.id, block.state});
    }

    // Clear all entries.
    void Clear();

    // Number of entries currently in the palette.
    uint16_t GetSize() const { return size_; }

    // Maximum number of entries.
    static constexpr uint16_t kMaxSize = static_cast<uint16_t>(kMaxPaletteSize);

    // Sentinel value indicating "not found" or "full".
    static constexpr uint8_t kInvalidIndex = 0xFF;

    // Direct access to entries (for serialization).
    const PaletteEntry* GetEntries() const { return entries_; }

private:
    PaletteEntry entries_[kMaxPaletteSize];
    uint16_t size_ = 0;
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_PALETTE_H
