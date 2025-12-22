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

/// Compute a useful 'sigma'. Refer to the function comment for details.
/// \param Extinction Extinction coefficient (optimized for [0.32, 32.0])
/// \param Scattering Albedo (optimized for [0.6, 0.99])
/// \return sigma
float3 EstimateMultiScatteringRadianceDecayCurve_GaussianModel_Sigma(float Extinction, float3 Albedo)
{
    // Assume isotropic and homogeneous scattering for simplicity.
    // The energy distribution from a certain depth z into a scattering volume is about propotional to:
    //      L(z) = A * exp(-(z/sigma)^2)
    // which is one day revealed to me by ChatGPT :) (and monte-carlo simualtion for validation ofcourse).
    // Thus, integrating the energy along a ray from 0 to zmax, weighted by transmittance and extinction coeff gives:
    // Lout = ∫[0,zmax] L(z) * Extinction * exp(-Extinction * z) dz
    //      = A * Extinction * ∫[0,zmax] exp(-(z/sigma)^2 - Extinction * z) dz
    // Taking tao = -1/sigma^2, Lout equals to
    // A * Extinction * sqrt(pi) / (2 * sqrt(-tao)) exp(Extinction^2 / (4 * tao)) * [erf((-2 * tao * zmax + Extinction) / (2 * sqrt(-tao))) - erf(Extinction / (2 * sqrt(-tao)))]
    // Thus, if we have Lout, sigma, Extinction, we can solve for A. Thus, we can estimate the radiated energy at any depth z.

    // This function effectively estimates sigma from extinction and albedo, based on trained data using rational function regression.

    // Clamp input to training data range
    Extinction = clamp(Extinction, 0.32f, 32.0f);
    Albedo = clamp(Albedo, 0.6f, 0.99f);

    // Shader-optimized Sigma function
    // Model: Rational function (2,1)
    // R² = 0.999259, RMSE = 2.474293e-02
    // Computation: ~18 ALU operations
    //
    // Normalization constants (from training data)
    static const float X_MEAN[2] = {11.97935484f, 0.81042807f};
    static const float X_SCALE[2] = {9.65651293f, 0.11513622f};
    static const float Y_MEAN = 0.49340295f;
    static const float Y_SCALE = 0.90917697f;

    // Rational function coefficients (normalized space)
    // Numerator: p0 + p1*e_n + p2*a_n + p3*e_n*a_n + p4*a_n^2
    static const float P[5] = {-0.42559311f, -0.43749772f, +0.02950238f,
                            +0.00028634f, +0.00981497f};

    // Denominator: 1 + q1*e_n
    static const float Q[1] = {+0.80612311f};

    // Normalize inputs
    float  e_norm = (Extinction - X_MEAN[0]) / X_SCALE[0];
    float3 a_norm = (Albedo - X_MEAN[1]) / X_SCALE[1];

    // Compute numerator: p[0] + p[1]*e_n + p[2]*a_n + p[3]*e_n*a_n + p[4]*a_n^2
    float3 a_sq = a_norm * a_norm;
    float3 numerator = P[0] + P[1] * e_norm + P[2] * a_norm + P[3] * (e_norm * a_norm) + P[4] * a_sq;

    // Compute denominator: 1 + q[0]*e_n
    float denominator = 1.0f + Q[0] * e_norm;

    // Safe division (avoid divide-by-zero)
    denominator = max(denominator, 1e-5f);

    // Compute normalized result
    float3 sigma_norm = numerator / denominator;

    // Denormalize output
    float3 sigma = sigma_norm * Y_SCALE + Y_MEAN;

    return sigma;
}


#endif