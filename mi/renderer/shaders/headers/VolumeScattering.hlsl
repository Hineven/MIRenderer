#ifndef VOLUME_SCATTERING_HLSL
#define VOLUME_SCATTERING_HLSL 

float SampleExponentialScatteringMedium (float ExtinctionFactor, float u) {
    float FreeFlightLength = - log(1 - u) / max(ExtinctionFactor, 1e-6f);
    return FreeFlightLength;
}

#endif