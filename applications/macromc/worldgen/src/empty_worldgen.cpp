/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "worldgen/empty_worldgen.h"
#include "registry/types.h"

MACROMC_WORLDGEN_NAMESPACE_BEGIN

using namespace MACROMC_WORLD_NAMESPACE;

void EmptyWorldgen::GenerateChunk(WorldShellData* /*shell*/,
                                  const ChunkCoord& /*coord*/,
                                  ChunkData* out_data) {
    // Fill entire chunk with air
    out_data->Fill(MACROMC_REGISTRY_NAMESPACE::kAirBlockId);
    out_data->SetState(ChunkState::kGenerated);
}

MACROMC_WORLDGEN_NAMESPACE_END
