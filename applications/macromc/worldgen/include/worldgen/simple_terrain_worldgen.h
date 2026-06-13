/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLDGEN_SIMPLE_TERRAIN_WORLDGEN_H
#define MACROMC_WORLDGEN_SIMPLE_TERRAIN_WORLDGEN_H

#include "worldgen/common.h"
#include "worldgen/worldgen.h"
#include "worldgen/perlin_noise.h"

MACROMC_WORLDGEN_NAMESPACE_BEGIN

// SimpleTerrainWorldgen: Basic noise-based terrain generator (Phase 1).
// Generates layered terrain: bedrock -> stone -> dirt -> grass -> air.
class SimpleTerrainWorldgen : public Worldgen {
public:
    explicit SimpleTerrainWorldgen(uint64_t seed = 0);
    ~SimpleTerrainWorldgen() override = default;

    void GenerateChunk(MACROMC_WORLD_NAMESPACE::WorldShellData* shell,
                       const MACROMC_WORLD_NAMESPACE::ChunkCoord& coord,
                       MACROMC_WORLD_NAMESPACE::ChunkData* out_data) override;

    MACROMC_WORLD_NAMESPACE::ShellCategory GetCategory() const override {
        return MACROMC_WORLD_NAMESPACE::ShellCategory::kTerrain;
    }

    // Noise parameters
    void SetFrequency(float freq) { frequency_ = freq; }
    void SetOctaves(int octaves) { octaves_ = octaves; }
    void SetBaseHeight(float height) { base_height_ = height; }
    void SetHeightVariation(float variation) { height_variation_ = variation; }

private:
    PerlinNoise noise_;
    float frequency_ = 0.01f;
    int octaves_ = 4;
    float base_height_ = 64.0f;
    float height_variation_ = 32.0f;

    float SampleHeight(float world_x, float world_z) const;
};

MACROMC_WORLDGEN_NAMESPACE_END

#endif // MACROMC_WORLDGEN_SIMPLE_TERRAIN_WORLDGEN_H
