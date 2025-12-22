#ifndef SOBOL_RESOURCES_HLSL
#define SOBOL_RESOURCES_HLSL

StructuredBuffer<uint> SobolBuffer;
StructuredBuffer<uint> SobolScramblingTileBuffer;

#define GOLDEN_RATIO 1.61803398874989484820f

float SamplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp(in int PixelX, in int PixelY, in int SampleIndex, in int SampleDimension)
{
    // A Low-Discrepancy Sampler that Distributes Monte Carlo Errors as a Blue Noise in Screen Space - Heitz etal

    // wrap arguments
    PixelX         = (PixelX & 127);
    PixelY         = (PixelY & 127);
    SampleIndex     = (SampleIndex & 255);
    SampleDimension = (SampleDimension & 255);

    // xor index based on optimized ranking (the latter is always 0)
    int RankedSampleIndex = SampleIndex;// ^ g_RankingTileBuffer[SampleDimension + (PixelX + PixelY * 128) * 8];

    // fetch value in sequence
    int value_index = SampleDimension + RankedSampleIndex * 256;
    int value_major_index = value_index / 4;
    int value_minor_index = value_index % 4;
    uint value_major = SobolBuffer[value_major_index];
    uint value = (value_major >> (value_minor_index * 8)) & 0xFF;

    // If the dimension is optimized, xor sequence value based on optimized scrambling
    int scrambling_index = (SampleDimension % 8) + (PixelX + PixelY * 128) * 8;
    int scrambling_major_index = scrambling_index / 4;
    int scrambling_minor_index = scrambling_index % 4;
    uint scrambling_major = SobolScramblingTileBuffer[scrambling_major_index];
    uint scrambling = (scrambling_major >> (scrambling_minor_index * 8)) & 0xFF;
    value = value ^ scrambling;

    // convert to float and return
    return (0.5f + value) / 256.0f;
}

float SampleSobol (int PixelX, int PixelY, int SampleIndex, int DimensionOffset) {
    float s = SamplerBlueNoiseErrorDistribution_128x128_OptimizedFor_2d2d2d2d_1spp(PixelX, PixelY, 0, DimensionOffset);
    // https://blog.demofox.org/2017/10/31/animating-noise-for-integration-over-time/
    return fmod(s + SampleIndex * GOLDEN_RATIO, 1.0f);
}

float2 SampleSobol2D (int PixelX, int PixelY, int SampleIndex, int DimensionOffset) {
    float2 s = float2(SampleSobol(PixelX, PixelY, 0, DimensionOffset), SampleSobol(PixelX, PixelY, 0, DimensionOffset + 1));
    // https://blog.demofox.org/2017/10/31/animating-noise-for-integration-over-time/
    return fmod(s + SampleIndex * GOLDEN_RATIO, 1.0f);
}

#undef GOLDEN_RATIO

#endif