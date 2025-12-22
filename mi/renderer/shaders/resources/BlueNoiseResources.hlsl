#ifndef BLUE_NOISE_SAMPLER_HLSL
#define BLUE_NOISE_SAMPLER_HLSL

[[vk::image_format(r32f)]]
Texture2D<float> BlueNoiseTexture;

#define GOLDEN_RATIO 1.61803398874989484820f
float SampleBlueNoise(in uint2 PixelCoords, in uint SampleIndex)
{
    // https://blog.demofox.org/2017/10/31/animating-noise-for-integration-over-time/
    float s = BlueNoiseTexture.Load(int3(PixelCoords, 0));

    return fmod(s + SampleIndex * GOLDEN_RATIO, 1.0f);
}

#undef GOLDEN_RATIO

#endif // BLUE_NOISE_SAMPLER_HLSL
