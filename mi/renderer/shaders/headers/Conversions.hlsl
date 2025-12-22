#ifndef CONVERTIONS_HLSL
#define CONVERTIONS_HLSL

float2 NDC2ToUV (float2 NDC2) {
    return float2(0.5f, -0.5f) * NDC2 + 0.5f.xx;
}

float2 UVToNDC2 (float2 UV) {
    return float2(2.f, -2.f) * (UV - 0.5f.xx);
}

#endif