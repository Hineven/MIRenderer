/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_WORLDGEN_H
#define MACROMC_WORLD_WORLDGEN_H

#include "world/common.h"
#include "world/types.h"
#include "world/shell_data.h"
#include "world/chunk_data.h"
#include <cstdint>

MACROMC_WORLD_NAMESPACE_BEGIN

// Worldgen base class
class Worldgen {
public:
    Worldgen() = default;
    virtual ~Worldgen() = default;
    
    // Generate chunk data (called on Worker Thread)
    virtual void GenerateChunk(WorldShellData* shell, 
                               const ChunkCoord& coord, 
                               ChunkData* out_data) = 0;
    
    // Get Worldgen category (for matching Shell category)
    virtual ShellCategory GetCategory() const = 0;
};

// Simple noise terrain generator (Phase 1)
class SimpleTerrainWorldgen : public Worldgen {
public:
    explicit SimpleTerrainWorldgen(uint64_t seed = 0);
    ~SimpleTerrainWorldgen() override = default;
    
    void GenerateChunk(WorldShellData* shell,
                       const ChunkCoord& coord,
                       ChunkData* out_data) override;
    
    ShellCategory GetCategory() const override { return ShellCategory::kTerrain; }
    
    // Noise parameters
    void SetFrequency(float freq) { frequency_ = freq; }
    void SetOctaves(int octaves) { octaves_ = octaves; }
    void SetBaseHeight(float height) { base_height_ = height; }
    void SetHeightVariation(float variation) { height_variation_ = variation; }
    
private:
    uint64_t seed_;
    float frequency_ = 0.01f;
    int octaves_ = 4;
    float base_height_ = 64.0f;
    float height_variation_ = 32.0f;
    
    // Noise sampling
    float SampleHeight(float world_x, float world_z) const;
    
    // Simple Perlin-like noise
    float Noise2D(float x, float z) const;
    float Fade(float t) const;
    float Lerp(float a, float b, float t) const;
    float Grad2D(int hash, float x, float z) const;
    
    // Permutation table
    static constexpr int kPermutationSize = 256;
    int perm_[512];  // Doubled permutation table for overflow handling
};

// Empty generator (for Empty Shell)
class EmptyWorldgen : public Worldgen {
public:
    void GenerateChunk(WorldShellData* shell,
                       const ChunkCoord& coord,
                       ChunkData* out_data) override;
    
    ShellCategory GetCategory() const override { return ShellCategory::kEmpty; }
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_WORLDGEN_H
