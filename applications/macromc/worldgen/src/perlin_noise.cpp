/*
 * Created: 2026/06/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "worldgen/perlin_noise.h"
#include <cmath>
#include <algorithm>

MACROMC_WORLDGEN_NAMESPACE_BEGIN

PerlinNoise::PerlinNoise(uint64_t seed) {
    // Initialize permutation table with seed-based shuffle
    for (int i = 0; i < kPermSize; ++i) {
        perm_[i] = i;
    }

    uint64_t s = seed;
    for (int i = kPermSize - 1; i > 0; --i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        int j = static_cast<int>(s % (i + 1));
        std::swap(perm_[i], perm_[j]);
    }

    // Duplicate for overflow handling
    for (int i = 0; i < kPermSize; ++i) {
        perm_[kPermSize + i] = perm_[i];
    }
}

float PerlinNoise::Sample2D(float x, float z) const {
    // Grid cell coordinates
    int xi = static_cast<int>(std::floor(x)) & 255;
    int zi = static_cast<int>(std::floor(z)) & 255;

    // Relative position within cell
    float xf = x - std::floor(x);
    float zf = z - std::floor(z);

    // Fade curves
    float u = Fade(xf);
    float v = Fade(zf);

    // Hash coordinates of cell corners
    int aa = perm_[perm_[xi] + zi];
    int ab = perm_[perm_[xi] + zi + 1];
    int ba = perm_[perm_[xi + 1] + zi];
    int bb = perm_[perm_[xi + 1] + zi + 1];

    // Blend gradients
    float x1 = Lerp(Grad2D(aa, xf, zf), Grad2D(ba, xf - 1, zf), u);
    float x2 = Lerp(Grad2D(ab, xf, zf - 1), Grad2D(bb, xf - 1, zf - 1), u);

    return Lerp(x1, x2, v);
}

float PerlinNoise::SampleOctaves2D(float x, float z, int octaves,
                                   float persistence, float lacunarity) const {
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float height = 0.0f;
    float max_amplitude = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        height += Sample2D(x * frequency, z * frequency) * amplitude;
        max_amplitude += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }

    // Normalize to [-1, 1]
    if (max_amplitude > 0.0f) {
        height /= max_amplitude;
    }

    return height;
}

float PerlinNoise::Fade(float t) const {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float PerlinNoise::Lerp(float a, float b, float t) const {
    return a + t * (b - a);
}

float PerlinNoise::Grad2D(int hash, float x, float z) const {
    int h = hash & 3;
    float u = (h & 1) == 0 ? x : -x;
    float v = (h & 2) == 0 ? z : -z;
    return u + v;
}

MACROMC_WORLDGEN_NAMESPACE_END
