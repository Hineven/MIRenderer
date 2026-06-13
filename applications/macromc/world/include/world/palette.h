/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_PALETTE_H
#define MACROMC_WORLD_PALETTE_H

#include "world/common.h"
#include "world/types.h"
#include "registry/types.h"
#include <cstdint>
#include <optional>

MACROMC_WORLD_NAMESPACE_BEGIN

// Palette: Chunk-level block type palette for palette compression.
// All SubChunks within a chunk share one Palette.
// Maps compact 8-bit indices (0~255) to actual BlockIds.
class Palette {
public:
    Palette();

    // Add a new block type to the palette. Returns the assigned index.
    // Returns kInvalidIndex if the palette is full (256 entries).
    uint8_t AddBlock(MACROMC_REGISTRY_NAMESPACE::BlockId block_id);

    // Look up the BlockId for a given palette index.
    MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockId(uint8_t index) const;

    // Find an existing entry or add a new one. Returns the index.
    // Returns kInvalidIndex if not found and palette is full.
    uint8_t FindOrAdd(MACROMC_REGISTRY_NAMESPACE::BlockId block_id);

    // Find an existing entry. Returns nullopt if not found.
    std::optional<uint8_t> Find(MACROMC_REGISTRY_NAMESPACE::BlockId block_id) const;

    // Clear all entries.
    void Clear();

    // Number of entries currently in the palette.
    uint16_t GetSize() const { return size_; }

    // Maximum number of entries.
    static constexpr uint16_t kMaxSize = static_cast<uint16_t>(kMaxPaletteSize);

    // Sentinel value indicating "not found" or "full".
    static constexpr uint8_t kInvalidIndex = 0xFF;

    // Direct access to entries (for serialization).
    const MACROMC_REGISTRY_NAMESPACE::BlockId* GetEntries() const { return entries_; }

private:
    MACROMC_REGISTRY_NAMESPACE::BlockId entries_[kMaxPaletteSize];
    uint16_t size_ = 0;
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_PALETTE_H
