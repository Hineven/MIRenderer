/*
 * Created: 2025/9/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <utility>
#include <vector>
#include <renderer/mi_noise.h>
#include <random>
#include <algorithm>

MI_NAMESPACE_BEGIN

float NoiseHelpers::HaltonValue(int index, int base) {
    float result = 0.0;
    float f = 1.0 / base;
    int i = index;
    while (i > 0) {
        result += f * (i % base);
        i /= base;
        f /= base;
    }
    return result;
}

std::vector<glm::vec2> NoiseHelpers::GenerateHaltonSequence2D (int count, int base1, int base2) {
    std::vector<glm::vec2> seq;
    seq.reserve(count);
    for (int i = 0; i < count; ++i) {
        float x = HaltonValue(i + 1, base1); // Halton 序列通常从 1 开始
        float y = HaltonValue(i + 1, base2);
        seq.emplace_back(x, y);
    }
    return seq;
}

// Perlin Noise
namespace {
    inline float fade(float t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }
    inline float lerp(float a, float b, float t) {
        return a + t * (b - a);
    }
    inline float grad1(int hash, float x) {
        return (hash & 1 ? x : -x);
    }
    inline float grad2(int hash, float x, float y) {
        int h = hash & 3;
        float u = h < 2 ? x : y;
        float v = h < 2 ? y : x;
        return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
    }
    static const int perm[512] = {
        151,160,137,91,90,15,
        131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
        190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
        88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
        77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
        102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208,89,18,169,200,196,
        135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
        5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
        223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
        129,22,39,253, 19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
        251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
        49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
        138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
        // repeat
        151,160,137,91,90,15,
        131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
        190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
        88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
        77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
        102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208,89,18,169,200,196,
        135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
        5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
        223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
        129,22,39,253, 19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,
        251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
        49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
        138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
    };
}

float NoiseHelpers::PerlinNoise1D(float x, int repeat) {
    if (repeat > 0) x = fmod(x, (float)repeat);
    int xi = (int)floor(x) & 255;
    float xf = x - floor(x);
    float u = fade(xf);
    int a = perm[xi];
    int b = perm[xi + 1];
    float gradA = grad1(a, xf);
    float gradB = grad1(b, xf - 1.0f);
    return lerp(gradA, gradB, u);
}

float NoiseHelpers::PerlinNoise2D(float x, float y, int repeat) {
    if (repeat > 0) {
        x = fmod(x, (float)repeat);
        y = fmod(y, (float)repeat);
    }
    int xi = (int)floor(x) & 255;
    int yi = (int)floor(y) & 255;
    float xf = x - floor(x);
    float yf = y - floor(y);
    float u = fade(xf);
    float v = fade(yf);
    int aa = perm[xi + perm[yi]];
    int ab = perm[xi + perm[yi + 1]];
    int ba = perm[xi + 1 + perm[yi]];
    int bb = perm[xi + 1 + perm[yi + 1]];
    float gradAA = grad2(aa, xf, yf);
    float gradBA = grad2(ba, xf - 1.0f, yf);
    float gradAB = grad2(ab, xf, yf - 1.0f);
    float gradBB = grad2(bb, xf - 1.0f, yf - 1.0f);
    float x1 = lerp(gradAA, gradBA, u);
    float x2 = lerp(gradAB, gradBB, u);
    return lerp(x1, x2, v);
}

std::vector<float> NoiseHelpers::BlueNoiseTexture2D(int width, int height, int seed) {
    int count = width * height;
    std::vector<float> blueNoise(count);
    std::mt19937 rng(seed);
    for (int i = 0; i < count; ++i) {
        blueNoise[i] = (float)rng() / (float)rng.max();
    }
    std::vector<float> filtered(count, 0.0f);
    // Low pass filter setup
    const int kernelRadius = 2;
    const float sigma = 1.0f;
    float kernel[2 * kernelRadius + 1][2 * kernelRadius + 1];
    float sum = 0.0f;
    for (int y = -kernelRadius; y <= kernelRadius; ++y) {
        for (int x = -kernelRadius; x <= kernelRadius; ++x) {
            float v = expf(-(x * x + y * y) / (2 * sigma * sigma));
            kernel[y + kernelRadius][x + kernelRadius] = v;
            sum += v;
        }
    }
    for (int y = 0; y < 2 * kernelRadius + 1; ++y)
        for (int x = 0; x < 2 * kernelRadius + 1; ++x)
            kernel[y][x] /= sum;
    // Apply filter
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            float acc = 0.0f;
            for (int ky = -kernelRadius; ky <= kernelRadius; ++ky) {
                for (int kx = -kernelRadius; kx <= kernelRadius; ++kx) {
                    int ni = std::clamp(i + kx, 0, width - 1);
                    int nj = std::clamp(j + ky, 0, height - 1);
                    acc += blueNoise[nj * width + ni] * kernel[ky + kernelRadius][kx + kernelRadius];
                }
            }
            filtered[j * width + i] = acc;
        }
    }
    // Normalize to [0, 1]
    float minVal = *std::min_element(filtered.begin(), filtered.end());
    float maxVal = *std::max_element(filtered.begin(), filtered.end());
    for (float& v : filtered) {
        v = (v - minVal) / (maxVal - minVal + 1e-6f);
    }
    return filtered;
}

MI_NAMESPACE_END