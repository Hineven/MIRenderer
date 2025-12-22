#ifndef COMMON_INDIRECT_LIGHTING_HLSL
#define COMMON_INDIRECT_LIGHTING_HLSL

// Quantilization
// May overflow if the radiance is too large (e.g. 1000)
uint QuantilizeRadiance (float V, float Noise = 0) {
    // Add precision check to prevent overflow and ensure stability
    V = clamp(V, 0.0f, 1000.0f);
    return floor(V * 16384.0f + Noise);
}
uint4 QuantilizeRadiance (float4 V, float Noise = 0) {
    return uint4(
        QuantilizeRadiance(V.x, Noise), QuantilizeRadiance(V.y, Noise),
        QuantilizeRadiance(V.z, Noise), QuantilizeRadiance(V.w, Noise));
}

float RecoverRadiance (uint V) {
    return float(V) / 16384;
}
float3 RecoverRadiance (uint3 V) {
    return float3(V) / 16384;
}
float4 RecoverRadiance (uint4 V) {
    return float4(V) / 16384;
}
uint QuantilizeWeight (float V, float Noise = 0) {
    // 2^17 = 131,072 (fp32: 2^23 precision)
    // Note: InvPdf < 100, no worries about overflowing
    return floor(V * 131072.f + Noise);
}
float RecoverWeight (uint V) {
    return float(V) / 131072.f;
}

#ifndef TILE_SIZE
#define TILE_SIZE 8
#endif

#ifdef TILE_SIZE
    #if TILE_SIZE != 8
    #error "TILE_SIZE must be 8"
    #endif
#endif

#define TILE_TEXEL_COUNT (TILE_SIZE * TILE_SIZE)
#define TILE_TEXEL_COUNT_L2 6

#ifndef WAVE_SIZE
#define WAVE_SIZE 32
#endif

#if TILE_TEXEL_COUNT % WAVE_SIZE != 0
#error "TILE_TEXEL_COUNT must be a multiple of WAVE_SIZE"
#endif

#endif