/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLDGEN_PERLIN_NOISE_H
#define MACROMC_WORLDGEN_PERLIN_NOISE_H

#include "worldgen/common.h"
#include <cstdint>

MACROMC_WORLDGEN_NAMESPACE_BEGIN

// PerlinNoise: Classic 2D Perlin noise implementation.
// Extracted from SimpleTerrainWorldgen for reuse by other generators.
class PerlinNoise {
public:
    explicit PerlinNoise(uint64_t seed = 0);

    // Sample 2D Perlin noise at (x, z).
    float Sample2D(float x, float z) const;

    // Sample with multiple octaves (fractal Brownian motion).
    // Returns normalized value in approximately [-1, 1].
    float SampleOctaves2D(float x, float z, int octaves,
                          float persistence = 0.5f, float lacunarity = 2.0f) const;

private:
    static constexpr int kPermSize = 256;
    int perm_[512];  // Doubled permutation table for overflow handling

    float Fade(float t) const;
    float Lerp(float a, float b, float t) const;
    float Grad2D(int hash, float x, float z) const;
};

MACROMC_WORLDGEN_NAMESPACE_END

#endif // MACROMC_WORLDGEN_PERLIN_NOISE_H
