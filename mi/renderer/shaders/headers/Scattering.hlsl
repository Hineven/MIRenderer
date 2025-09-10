#ifndef SCATTERING_HLSL
#define SCATTERING_HLSL

float HenyeyGreensteinPhaseFunction(float Cosine, float g) {
    // Henyey-Greenstein phase function
    float g2 = g * g;
    float denom_term = 1.f + g2 + 2.f * g * Cosine;
    return (1.f - g2) / (4.f * PI * denom_term * sqrt(denom_term));
}

// Samples a direction based on the Henyey-Greenstein phase function.
// g: anisotropy parameter, in [-1, 1].
// u: 2D uniform random sample.
// pdf: output spherical PDF for the sampled direction.
// Returns a sampled direction in a local coordinate system (around Z-axis).
float3 SampleHenyeyGreenstein(float g, float2 u, out float Pdf) {
    float CosTheta;
    // Handle the isotropic case (g=0) to avoid division by zero and for precision.
    if (abs(g) < 1e-4f) {
        CosTheta = 1.f - 2.f * u.x;
    } else {
        float g2 = g * g;
        float term = (1.f - g2) / (1.f - g + 2.f * g * u.x);
        CosTheta = (1.f / (2.f * g)) * (1.f + g2 - term * term);
    }

    // Convert spherical coordinates to a Cartesian direction vector.
    float SinTheta = sqrt(max(0.f, 1.f - CosTheta * CosTheta));
    float Phi = 2.f * PI * u.y;

    float3 sample_dir = float3(
        cos(Phi) * SinTheta,
        sin(Phi) * SinTheta,
        CosTheta
    );

    // The PDF of this sample is the HG function itself.
    Pdf = HenyeyGreensteinPhaseFunction(CosTheta, g);

    return sample_dir;
}

#endif // SCATTERING_HLSL