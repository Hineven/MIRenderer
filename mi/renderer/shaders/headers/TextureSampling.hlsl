#ifndef TEXTURE_SAMPLING_HLSL
#define TEXTURE_SAMPLING_HLSL


float4 SampleTexture(Texture2D<float4> Tex, SamplerState Sampler, float2 UV, float LOD)
{
#ifdef MI_GRAPHICS_SHADER
    if(LOD == -1) {
        return Tex.Sample(Sampler, UV);
    } else {
        return Tex.SampleLevel(Sampler, UV, LOD);
    }
#else
    return Tex.SampleLevel(Sampler, UV, LOD);
#endif
}

float2 SampleTexture(Texture2D<float2> Tex, SamplerState Sampler, float2 UV, float LOD)
{
#ifdef MI_GRAPHICS_SHADER
    if(LOD == -1) {
        return Tex.Sample(Sampler, UV);
    } else {
        return Tex.SampleLevel(Sampler, UV, LOD);
    }
#else
    return Tex.SampleLevel(Sampler, UV, LOD);
#endif
}


#endif