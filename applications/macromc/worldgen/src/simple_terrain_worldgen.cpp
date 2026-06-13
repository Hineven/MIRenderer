/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "worldgen/simple_terrain_worldgen.h"
#include "registry/block_registry.h"
#include "world/types.h"
#include <algorithm>

MACROMC_WORLDGEN_NAMESPACE_BEGIN

using namespace MACROMC_WORLD_NAMESPACE;

SimpleTerrainWorldgen::SimpleTerrainWorldgen(uint64_t seed)
    : noise_(seed) {
}

void SimpleTerrainWorldgen::GenerateChunk(WorldShellData* shell,
                                          const ChunkCoord& coord,
                                          ChunkData* out_data) {
    // Get block IDs from registry
    auto& reg = MACROMC_REGISTRY_NAMESPACE::GetGlobalBlockRegistry();
    auto bedrock_id = reg.GetBlockId("bedrock");
    auto stone_id = reg.GetBlockId("stone");
    auto dirt_id = reg.GetBlockId("dirt");
    auto grass_id = reg.GetBlockId("grass");

    // Chunk base world position
    float chunk_base_x = static_cast<float>(coord.x * static_cast<int>(kChunkSizeX));
    float chunk_base_z = static_cast<float>(coord.z * static_cast<int>(kChunkSizeZ));

    // Generate terrain for each column
    for (uint32_t lx = 0; lx < kChunkSizeX; ++lx) {
        for (uint32_t lz = 0; lz < kChunkSizeZ; ++lz) {
            float world_x = chunk_base_x + static_cast<float>(lx);
            float world_z = chunk_base_z + static_cast<float>(lz);

            // Sample height using octave noise
            float height_sample = SampleHeight(world_x, world_z);
            int terrain_height = static_cast<int>(base_height_ + height_sample * height_variation_);
            terrain_height = std::clamp(terrain_height, 1, static_cast<int>(kChunkSizeY) - 1);

            // Fill column
            for (uint32_t y = 0; y < kChunkSizeY; ++y) {
                MACROMC_REGISTRY_NAMESPACE::BlockId block_id;

                if (y == 0) {
                    block_id = bedrock_id;
                } else if (y < static_cast<uint32_t>(terrain_height) - 4) {
                    block_id = stone_id;
                } else if (y < static_cast<uint32_t>(terrain_height)) {
                    block_id = dirt_id;
                } else if (y == static_cast<uint32_t>(terrain_height)) {
                    block_id = grass_id;
                } else {
                    block_id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
                }

                out_data->SetBlock(lx, y, lz, block_id);
            }
        }
    }

    out_data->SetState(ChunkState::kGenerated);
}

float SimpleTerrainWorldgen::SampleHeight(float world_x, float world_z) const {
    return noise_.SampleOctaves2D(world_x * frequency_, world_z * frequency_,
                                  octaves_);
}

MACROMC_WORLDGEN_NAMESPACE_END
