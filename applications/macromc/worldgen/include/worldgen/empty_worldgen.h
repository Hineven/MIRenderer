/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLDGEN_EMPTY_WORLDGEN_H
#define MACROMC_WORLDGEN_EMPTY_WORLDGEN_H

#include "worldgen/common.h"
#include "worldgen/worldgen.h"

MACROMC_WORLDGEN_NAMESPACE_BEGIN

// EmptyWorldgen: Generates chunks filled entirely with air.
// Used for Empty Shell category.
class EmptyWorldgen : public Worldgen {
public:
    void GenerateChunk(MACROMC_WORLD_NAMESPACE::WorldShellData* shell,
                       const MACROMC_WORLD_NAMESPACE::ChunkCoord& coord,
                       MACROMC_WORLD_NAMESPACE::ChunkData* out_data) override;

    MACROMC_WORLD_NAMESPACE::ShellCategory GetCategory() const override {
        return MACROMC_WORLD_NAMESPACE::ShellCategory::kEmpty;
    }
};

MACROMC_WORLDGEN_NAMESPACE_END

#endif // MACROMC_WORLDGEN_EMPTY_WORLDGEN_H
