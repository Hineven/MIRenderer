/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLDGEN_WORLDGEN_H
#define MACROMC_WORLDGEN_WORLDGEN_H

#include "worldgen/common.h"
#include "world/types.h"
#include "world/shell_data.h"
#include "world/chunk_data.h"
#include <cstdint>

MACROMC_WORLDGEN_NAMESPACE_BEGIN

// Worldgen base class: Interface for terrain generation algorithms.
// Implementations run on Worker Threads and write block data into ChunkData.
class Worldgen {
public:
    Worldgen() = default;
    virtual ~Worldgen() = default;

    // Generate chunk data (called on Worker Thread).
    // The implementation should fill out_data with block information.
    virtual void GenerateChunk(MACROMC_WORLD_NAMESPACE::WorldShellData* shell,
                               const MACROMC_WORLD_NAMESPACE::ChunkCoord& coord,
                               MACROMC_WORLD_NAMESPACE::ChunkData* out_data) = 0;

    // Get the ShellCategory this generator is designed for.
    virtual MACROMC_WORLD_NAMESPACE::ShellCategory GetCategory() const = 0;
};

MACROMC_WORLDGEN_NAMESPACE_END

#endif // MACROMC_WORLDGEN_WORLDGEN_H
