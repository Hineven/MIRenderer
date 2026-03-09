/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/worldgen.h"
#include "registry/block_registry.h"
#include <cmath>
#include <algorithm>

MACROMC_WORLD_NAMESPACE_BEGIN

// =============================================================================
// SimpleTerrainWorldgen Implementation
// =============================================================================

SimpleTerrainWorldgen::SimpleTerrainWorldgen(uint64_t seed) 
    : seed_(seed) {
    // Initialize permutation table with seed-based shuffle
    // Start with identity permutation
    for (int i = 0; i < kPermutationSize; ++i) {
        perm_[i] = i;
    }
    
    // Shuffle using seed
    uint64_t s = seed;
    for (int i = kPermutationSize - 1; i > 0; --i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        int j = static_cast<int>(s % (i + 1));
        std::swap(perm_[i], perm_[j]);
    }
    
    // Duplicate for overflow handling
    for (int i = 0; i < kPermutationSize; ++i) {
        perm_[kPermutationSize + i] = perm_[i];
    }
}

void SimpleTerrainWorldgen::GenerateChunk(WorldShellData* shell,
                                          const ChunkCoord& coord,
                                          ChunkData* out_data) {
    // Ensure blocks are allocated
    out_data->AllocateBlocks();
    
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
            
            // Sample height
            float height_sample = SampleHeight(world_x, world_z);
            int terrain_height = static_cast<int>(base_height_ + height_sample * height_variation_);
            terrain_height = std::clamp(terrain_height, 1, static_cast<int>(kChunkSizeY) - 1);
            
            // Fill column
            for (uint32_t y = 0; y < kChunkSizeY; ++y) {
                BlockData block;
                
                if (y == 0) {
                    // Bedrock at bottom
                    block.id = bedrock_id;
                } else if (y < static_cast<uint32_t>(terrain_height) - 4) {
                    // Stone layer
                    block.id = stone_id;
                } else if (y < static_cast<uint32_t>(terrain_height)) {
                    // Dirt layer
                    block.id = dirt_id;
                } else if (y == static_cast<uint32_t>(terrain_height)) {
                    // Grass surface
                    block.id = grass_id;
                } else {
                    // Air above terrain
                    block.id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
                }
                
                out_data->SetBlock(lx, y, lz, block);
            }
        }
    }
    
    out_data->SetState(ChunkState::kGenerated);
}

float SimpleTerrainWorldgen::SampleHeight(float world_x, float world_z) const {
    float amplitude = 1.0f;
    float frequency = frequency_;
    float height = 0.0f;
    float max_amplitude = 0.0f;
    
    for (int i = 0; i < octaves_; ++i) {
        height += Noise2D(world_x * frequency, world_z * frequency) * amplitude;
        max_amplitude += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    
    // Normalize to [-1, 1]
    if (max_amplitude > 0.0f) {
        height /= max_amplitude;
    }
    
    return height;
}

float SimpleTerrainWorldgen::Noise2D(float x, float z) const {
    // Grid cell coordinates
    int xi = static_cast<int>(std::floor(x)) & 255;
    int zi = static_cast<int>(std::floor(z)) & 255;
    
    // Relative position within cell
    float xf = x - std::floor(x);
    float zf = z - std::floor(z);
    
    // Fade curves
    float u = Fade(xf);
    float v = Fade(zf);
    
    // Hash coordinates of cube corners
    int aa = perm_[perm_[xi] + zi];
    int ab = perm_[perm_[xi] + zi + 1];
    int ba = perm_[perm_[xi + 1] + zi];
    int bb = perm_[perm_[xi + 1] + zi + 1];
    
    // Blend gradients
    float x1 = Lerp(Grad2D(aa, xf, zf), Grad2D(ba, xf - 1, zf), u);
    float x2 = Lerp(Grad2D(ab, xf, zf - 1), Grad2D(bb, xf - 1, zf - 1), u);
    
    return Lerp(x1, x2, v);
}

float SimpleTerrainWorldgen::Fade(float t) const {
    return t * t * t * (t * (t * 6 - 15) + 10);
}

float SimpleTerrainWorldgen::Lerp(float a, float b, float t) const {
    return a + t * (b - a);
}

float SimpleTerrainWorldgen::Grad2D(int hash, float x, float z) const {
    // Convert low 2 bits to gradient direction
    int h = hash & 3;
    float u = (h & 1) == 0 ? x : -x;
    float v = (h & 2) == 0 ? z : -z;
    return u + v;
}

// =============================================================================
// EmptyWorldgen Implementation
// =============================================================================

void EmptyWorldgen::GenerateChunk(WorldShellData* shell,
                                  const ChunkCoord& coord,
                                  ChunkData* out_data) {
    // Fill with air
    BlockData air;
    air.id = MACROMC_REGISTRY_NAMESPACE::kAirBlockId;
    out_data->Fill(air);
    out_data->SetState(ChunkState::kGenerated);
}

MACROMC_WORLD_NAMESPACE_END
