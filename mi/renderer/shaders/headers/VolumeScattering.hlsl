#ifndef VOLUME_SCATTERING_HLSL
#define VOLUME_SCATTERING_HLSL 

float SampleExponentialScatteringMedium (float ExtinctionFactor, float u) {
    float FreeFlightLength = - log(1 - u) / max(ExtinctionFactor, 1e-6f);
    return FreeFlightLength;
}

// Return the transmittance after traveling through a distance 'Depth' in the medium
float IntegrateExponentialScatteringMedium (float ExtinctionFactor, float Depth) {
    return exp(-ExtinctionFactor * max(Depth, 0));
}

#endif