/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_BLOCK_DATA_H
#define MACROMC_WORLD_BLOCK_DATA_H

#include "world/common.h"
#include "registry/types.h"
#include <cstdint>

MACROMC_WORLD_NAMESPACE_BEGIN

// BlockData: A block identity = {id, state}.
//
// `id` selects the BlockDefinition (type). `state` carries per-instance state
// bits (orientation, growth stage, connection flags, ...). State semantics are
// defined per block; 0 means "default / no state".
//
// ChunkData stores palette-compressed indices via SubChunk + Palette. The
// Palette entry type is {id, state} so two instances of the same block id with
// different state occupy distinct palette slots (e.g. top-slab vs bottom-slab).
// BlockData is the value type read/written through the ChunkData API.
struct BlockData {
    MACROMC_REGISTRY_NAMESPACE::BlockId id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
    uint16_t state = 0; // block state bits; 0 = default/none

    bool IsAir() const { return id == MACROMC_REGISTRY_NAMESPACE::kAirBlockId; }

    bool operator==(const BlockData&) const = default;
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_BLOCK_DATA_H
