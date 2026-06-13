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

// BlockData: Lightweight block reference (NOT stored in ChunkData).
//
// ChunkData stores palette-compressed indices via SubChunk + Palette.
// BlockData is a convenience struct for passing block information around
// (e.g., in worldgen, gameplay queries, etc.)
//
// Note: Block state data (direction, growth stage, signal, etc.) is NOT included
// here. When block states are needed in the future, use a separate sparse storage
// (e.g., hash map keyed by block position) to keep the core path lightweight.
struct BlockData {
    MACROMC_REGISTRY_NAMESPACE::BlockId id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;

    bool IsAir() const { return id == MACROMC_REGISTRY_NAMESPACE::kAirBlockId; }
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_BLOCK_DATA_H
