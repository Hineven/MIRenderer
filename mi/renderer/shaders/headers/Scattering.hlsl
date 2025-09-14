#ifndef SCATTERING_HLSL
#define SCATTERING_HLSL

#include "MathConstants.hlsl"
#include "Radiometry.hlsl"

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

// Copy pasted from Capsaicin for commonly used scattering models

/**********************************************************************
Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
********************************************************************/
/**
 * Calculates schlick fresnel term.
 * @param F0    The fresnel reflectance an grazing angle.
 * @param dotHV The dot product of the half-vector and view direction (range [-1, 1]).
 * @return The calculated fresnel term.
 */
float3 Fresnel(float3 F0, float dotHV)
{
    // The half-vector may be incorrectly flipped or invisible to the view direction in some cases, and thus
    // dotHV may be negative. For this case, we use abs(dotHV) to correct flipping and avoid NaN.
    float3 F90 = 1.0f.xxx;
    return F0 + (F90 - F0) * pow(1.0f - saturate(abs(dotHV)), 5.0f);
}

/**
 * Calculates the amount to modify the diffuse component of a combined BRDF.
 * @param f     Pre-calculated fresnel value.
 * @param dotHV The dot product of the half-vector and view direction (range [-1, 1]).
 * @return The amount to modify diffuse component by.
 */
float3 DiffuseCompensationTerm(float3 f, float dotHV)
{
    // PBR Diffuse Lighting for GGX + Smith Microsurfaces - Hammon 2017

    // The half-vector may be incorrectly flipped or invisible to the view direction in some cases, and thus
    // dotHV may be negative. For this case, we use abs(dotHV) to correct flipping and avoid NaN.
    return (1.0f.xxx - f) * 1.05 * (1.0f - pow(1.0f - saturate(abs(dotHV)), 5.0f));
}

/**
 * Calculates the amount to modify the diffuse component of a combined BRDF.
 * @param F0    The fresnel reflectance at grazing angle.
 * @param dotHV The dot product of the half-vector and view direction (range [-1, 1]).
 * @return The amount to modify diffuse component by.
 */
float3 DiffuseCompensation(float3 F0, float dotHV)
{
    return DiffuseCompensationTerm(Fresnel(F0, dotHV), dotHV);
}

/**
 * Evaluate the Trowbridge-Reitz Normal Distribution Function.
 * @param roughnessAlphaSqr The NDF roughness value squared.
 * @param dotNH             The dot product of the normal and half vector (range [-1, 1]).
 * @return The calculated NDF value.
 */
float EvaluateNDFTrowbridgeReitz(float roughnessAlphaSqr, float dotNH)
{
    // Heaviside function for microfacet normal in the upper hemisphere.
    if (dotNH < 0.0f)
    {
        return 0.0f;
    }

    float denom = dotNH * dotNH * (roughnessAlphaSqr - 1.0f) + 1.0f;
    float d = roughnessAlphaSqr / (PI * denom * denom);
    return d;
}

/**
 * Evaluate the GGX Visibility function.
 * @param roughnessAlphaSqr The GGX roughness value squared.
 * @param dotNL             The dot product of the normal and light direction (range [-1, 1]).
 * @param dotNV             The dot product of the normal and view direction (range [-1, 1]).
 * @return The calculated visibility value.
 */
float EvaluateVisibilityGGX(float roughnessAlphaSqr, float dotNL, float dotNV)
{
    // The masking-shadowing function is indefinite for back-facing shading normals.
    // So we use abs(dotNL) and abs(dotNV) for this case.
    // This is hacky, but still satisfies the reciprocity and energy conservation.
    float rMod = 1.0f - roughnessAlphaSqr;
    float recipG1 = abs(dotNL) + sqrt(roughnessAlphaSqr + (rMod * dotNL * dotNL));
    float recipG2 = abs(dotNV) + sqrt(roughnessAlphaSqr + (rMod * dotNV * dotNV));
    float recipV = recipG1 * recipG2;
    return recipV;
}

/**
 * Evaluate the GGX BRDF.
 * @param roughnessAlpha    The GGX roughness value.
 * @param roughnessAlphaSqr The GGX roughness value squared.
 * @param F0                The fresnel reflectance at grazing angle.
 * @param dotHV             The dot product of the half-vector and view direction (range [-1, 1]).
 * @param dotNH             The dot product of the normal and half vector (range [-1, 1]).
 * @param dotNL             The dot product of the normal and light direction (range [-1, 1]).
 * @param dotNV             The dot product of the normal and view direction (range [-1, 1]).
 * @param fresnelOut        (Out) The returned fresnel value.
 * @return The calculated reflectance.
 */
float3 EvaluateGGX(float roughnessAlpha, float roughnessAlphaSqr, float3 F0, float dotHV, float dotNH, float dotNL, float dotNV, out float3 fOut)
{
    // Calculate Fresnel
    fOut = Fresnel(F0, dotHV);

    // Calculate Trowbridge-Reitz Distribution function
    float d = EvaluateNDFTrowbridgeReitz(roughnessAlphaSqr, dotNH);

    // Calculate GGX Visibility function
    float recipV = EvaluateVisibilityGGX(roughnessAlphaSqr, dotNL, dotNV);

    return (fOut * d) / recipV;
}

/**
 * Evaluate the Lambert BRDF.
 * @param albedo The diffuse colour term.
 * @return The calculated reflectance.
 */
float3 EvaluateLambert(float3 albedo)
{
    return albedo / PI;
}


/**
 * Calculate a sampled direction for the GGX BRDF using Heitz VNDF sampling.
 * @note All calculations are done in the surfaces local tangent space. Only allows
 *  for view directions in top hemisphere (N.V>0).
 * @param roughnessAlpha The GGX roughness value.
 * @param localView      Outgoing ray view direction (in local space).
 * @param samples        Random number samples used to sample BRDF.
 * @return The sampled direction in local space.
 */
float3 SampleGGXVNDFUpper(float roughnessAlpha, float3 localView, float2 samples)
{
    // A Simpler and Exact Sampling Routine for the GGX Distribution of Visible Normals - Heitz 2017

    // Stretch the view vector as if roughness==1
    float3 stretchedView = normalize(float3(roughnessAlpha * localView.xy, localView.z));
    // Create an orthonormal basis (requires that viewDirection is always above surface i.e viewDirection.geomNormal > 0))
    float3 T1 = (stretchedView.z < 0.9999) ? normalize(cross(stretchedView, float3(0.0f, 0.0f, 1.0f))) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(T1, stretchedView);
    // Sample a disk with each half of the disk weighted proportionally to its projection onto stretchedView
    //  This creates polar coordinates (r, phi)
    float a = 1.0f / (1.0f + stretchedView.z);
    float r = sqrt(samples.x);
    float phi = (samples.y < a) ? samples.y / a * PI : PI + (samples.y - a) / (1.0f - a) * PI;
    float P1 = r * cos(phi);
    float P2 = r * sin(phi) * ((samples.y < a) ? 1.0f : stretchedView.z);
    // Calculate normal (defined in the stretched tangent space)
    float3 normal = P1 * T1 + P2 * T2 + sqrt(max(0.0f, 1.0f - P1 * P1 - P2 * P2)) * stretchedView;
    // Convert normal to un-stretched and normalise
    normal = normalize(float3(roughnessAlpha * normal.xy, max(0.0f, normal.z)));
    return normal;
}

/**
 * Calculate a sampled direction for the GGX BRDF using Heitz VNDF sampling of full sphere.
 * @note All calculations are done in the surfaces local tangent space.
 * @param roughnessAlpha The GGX roughness value.
 * @param localView      Outgoing ray view direction (in local space).
 * @param samples        Random number samples used to sample BRDF.
 * @return The sampled direction in local space.
 */
float3 SampleGGXVNDFFull(float roughnessAlpha, float3 localView, float2 samples)
{
    // Sampling the GGX Distribution of Visible Normals - Heitz 2018

    // Stretch the view vector as if roughness==1
    float3 stretchedView = normalize(float3(roughnessAlpha * localView.xy, localView.z));
    // Create an orthonormal basis (with special case if cross product is zero)
    float lengthSqr = dot(stretchedView.xy, stretchedView.xy);
    float3 T1 = lengthSqr > 0 ? float3(-stretchedView.y, stretchedView.x, 0.0f) * rsqrt(lengthSqr) : float3(1.0f, 0.0f, 0.0f);
    float3 T2 = cross(stretchedView, T1);
    // Sample a disk with each half of the disk weighted proportionally to its projection onto stretchedView
    //  This creates polar coordinates (r, phi)
    float r = sqrt(samples.x);
    float phi = 2.0f * PI * samples.y;
    float P1 = r * cos(phi);
    float P2 = r * sin(phi);
    float s = 0.5f * (1.0f + stretchedView.z);
    P2 = (1.0f - s) * sqrt(1.0f - P1 * P1) + s * P2;
    // Calculate normal (defined in the stretched tangent space)
    float3 normal = P1 * T1 + P2 * T2 + sqrt(max(0.0f, 1.0f - P1 * P1 - P2 * P2)) * stretchedView;
    // Convert normal to un-stretched and normalise
    normal = normalize(float3(roughnessAlpha * normal.xy, max(0.0f, normal.z)));
    return normal;
}

/**
 * Calculate a sampled direction for the GGX BRDF using spherical cap sampling.
 * @note All calculations are done in the surfaces local tangent space.
 * @param roughnessAlpha The GGX roughness value.
 * @param localView      Outgoing ray view direction (in local space).
 * @param samples        Random number samples used to sample BRDF.
 * @return The sampled direction in local space.
 */
float3 SampleGGXVNDFSphericalCap(float roughnessAlpha, float3 localView, float2 samples)
{
    // Sampling Visible GGX Normals with Spherical Caps - Jonathan Dupuy and Anis Benyoub 2023
    // https://doi.org/10.1111/cgf.14867

    // Stretch the view vector as if roughness==1
    float3 wiStd = normalize(float3(roughnessAlpha * localView.xy, localView.z));

    float phi = 2.0f * PI * samples.y;
    float z = mad(-wiStd.z, samples.x, 1.0f - samples.x);
    float sinTheta = sqrt(saturate(1.0f - z * z));
    float x = sinTheta * cos(phi);
    float y = sinTheta * sin(phi);
    float3 c = float3(x, y, z);
    float3 wmStd = c + wiStd;

    // Convert normal to un-stretched and normalise
    float3 wm = normalize(float3(roughnessAlpha * wmStd.xy, wmStd.z));
    return wm;
}

/**
 * Calculate a sampled direction for the GGX BRDF using bounded spherical cap sampling.
 * @note All calculations are done in the surfaces local tangent space.
 * @param roughnessAlpha The GGX roughness value.
 * @param localView      Outgoing ray view direction (in local space).
 * @param samples        Random number samples used to sample BRDF.
 * @return The sampled direction in local space.
 */
float3 SampleGGXVNDFBounded(float roughnessAlpha, float3 localView, float2 samples)
{
    // Bounded VNDF Sampling for Smith-GGX Reflections - Kenta Eto and Yusuke Tokuyoshi 2023
    // https://doi.org/10.1145/3610543.3626163

    // Stretch the view vector as if roughness==1
    float3 wiStd = normalize(float3(roughnessAlpha * localView.xy, localView.z));

    float phi = 2.0f * PI * samples.y;
    float a = roughnessAlpha; // Use a = saturate(min(roughnessAlpha.x, roughnessAlpha.y)) for anisotropic roughness.
    float s = 1.0f + sign(1.0f - a) * length(float2(localView.x, localView.y));
    float a2 = a * a;
    float s2 = s * s;
    float k = (1.0f - a2) * s2 / (s2 + a2 * localView.z * localView.z);
    float b = localView.z > 0.0f ? k * wiStd.z : wiStd.z;

    float z = mad(-b, samples.x, 1.0f - samples.x);
    float sinTheta = sqrt(saturate(1.0f - z * z));
    float x = sinTheta * cos(phi);
    float y = sinTheta * sin(phi);
    float3 c = float3(x, y, z);
    float3 wmStd = c + wiStd;

    // Convert normal to un-stretched and normalise
    float3 wm = normalize(float3(roughnessAlpha * wmStd.xy, wmStd.z));
    return wm;
}

/**
 * Calculate a random direction around a +z axis oriented hemisphere.
 * @note Uses a cosine-weighted distribution
 * @param samples Random number samples used to generate direction.
 * @return The sampled direction in local space.
 */
float3 SampleHemisphere(float2 samples)
{
    // Ray Tracing Gems - Sampling Transformations Zoo - Shirley
    float a = sqrt(samples.x);
    float b = TWO_PI * samples.y;
    return float3(a * cos(b), a * sin(b), sqrt(1.0f - samples.x));
}

/**
 * Calculate a sampled direction for the GGX BRDF.
 * @note All calculations are done in the surfaces local tangent space
 * @param roughnessAlpha The GGX roughness value.
 * @param localView      Outgoing ray view direction (in local space).
 * @param samples        Random number samples used to sample BRDF.
 * @return The sampled direction in local space.
 */
float3 SampleGGX(float roughnessAlpha, float3 localView, float2 samples)
{
    // Sample the local space micro-facet normal
    // float3 sampledNormal = SampleGGXVNDFUpper(roughnessAlpha, localView, samples);
    // float3 sampledNormal = SampleGGXVNDFFull(roughnessAlpha, localView, samples); // Use this for shading normals.
    // float3 sampledNormal = SampleGGXVNDFSphericalCap(roughnessAlpha, localView, samples);
    float3 sampledNormal = SampleGGXVNDFBounded(roughnessAlpha, localView, samples);

    // Calculate light direction
    float3 sampledLight = reflect(-localView, sampledNormal);
    return sampledLight;
}

/**
 * Calculate the BVNDF sampling PDF for given values for the GGX BRDF.
 * @param roughnessAlphaSqr The GGX roughness value squared.
 * @param dotNH             The dot product of the local normal and half vector (range [-1, 1]).
 * @return The calculated PDF.
 */
float SampleGGXVNDFPDF(float roughnessAlphaSqr, float dotNH, float dotNV)
{
    // Calculate NDF function
    float d = EvaluateNDFTrowbridgeReitz(roughnessAlphaSqr, dotNH);

    float dotNV2 = saturate(dotNV * dotNV);
    float s = roughnessAlphaSqr * (1.0f - dotNV2);
    float t = sqrt(s + dotNV2);

    // Calculate the normalization factor considering backfacing shading normals.
    // [Tokuyoshi 2021 "Unbiased VNDF Sampling for Backfacing Shading Normals"]
    // https://gpuopen.com/download/publications/Unbiased_VNDF_Sampling_for_Backfacing_Shading_Normals.pdf
    // The normalization factor for the Smith-GGX VNDF is (t + dotNV) / 2.
    // But t + dotNV can have catastrophic cancellation when dotNV < 0.
    // Therefore, we avoid the catastrophic cancellation by equivarently rewriting the form as follows:
    // t + dotNV = (t + dotNV) * (t - dotNV) / (t - dotNV) = s / (t - dotNV).
    // In this implementation, we clamp dotNV for the case in abs(dotNV) > 1.
    float recipNormFactor = dotNV >= 0.0f ? t + saturate(dotNV) : s / (t + saturate(abs(dotNV)));
    return d / (2.0f * recipNormFactor);
}

/**
 * Calculate the BVNDF sampling PDF for given values for the GGX BRDF.
 * @param roughnessAlphaSqr The GGX roughness value squared.
 * @param dotNH             The dot product of the local normal and half vector (range [-1, 1]).
 * @param localView         Outgoing ray view direction (in local space).
 * @return The calculated PDF.
 */
float SampleGGXVNDFBoundedPDF(float roughnessAlphaSqr, float dotNH, float3 localView)
{
    // Kenta Eto and Yusuke Tokuyoshi. 2023. Bounded VNDF Sampling for Smith-GGX Reflections. SIGGRAPH Asia 2023 Technical Communications. https://doi.org/10.1145/3610543.3626163
    float ndf = EvaluateNDFTrowbridgeReitz(roughnessAlphaSqr, dotNH);
    float roughnessAlpha = sqrt(roughnessAlphaSqr);
    float2 ai = roughnessAlpha * localView.xy;
    float len2 = dot(ai, ai);
    float t = sqrt(len2 + localView.z * localView.z);
    if (localView.z >= 0.0f)
    {
        float a = roughnessAlpha; // Use a = saturate(min(roughnessAlpha.x, roughnessAlpha.y)) for anisotropic roughness.
        float s = 1.0f + sign(1.0f - a) * length(float2(localView.x, localView.y));
        float a2 = a * a;
        float s2 = s * s;
        float k = (1.0f - a2) * s2 / (s2 + a2 * localView.z * localView.z);
        return ndf / (2.0f * (k * localView.z + t));
    }
    return ndf * (t - localView.z) / (2.0f * len2);
}

/**
 * Calculate the PDF for given values for the GGX BRDF.
 * @param roughnessAlphaSqr The GGX roughness value squared.
 * @param dotNH             The dot product of the local normal and half vector (range [-1, 1]).
 * @param dotNV             The dot product of the local normal and light direction (range [-1, 1]).
 * @param localView         Outgoing ray view direction (in local space).
 * @return The calculated PDF.
 */
float SampleGGXPDF(float roughnessAlphaSqr, float dotNH, float dotNV, float3 localView)
{
    // Can change the sampling method
    // return SampleGGXVNDFPDF(roughnessAlphaSqr, dotNH, dotNV);
    return SampleGGXVNDFBoundedPDF(roughnessAlphaSqr, dotNH, localView);
}

/**
 * Calculate the approximate direction of the specular peak.
 * @note This can be used in place of a light direction vector when calculating
 * shading half-vectors when the light direction is unknown.
 * @param normal         Shading normal vector at current position (must be normalised).
 * @param viewDirection  Outgoing ray view direction (must be normalised).
 * @param roughness      The GGX perceptual roughness (roughness = sqrt(roughnessAlpha).
 * @return The calculated direction.
 */
float3 CalculateGGXSpecularDirection(float3 normal, float3 viewDirection, float roughness)
{
    // Moving Frostbite to Physically Based Rendering 3.0, page 69
    float3 reflection = reflect(-viewDirection, normal);
    float smoothness = saturate(1 - roughness);
    float lerpFactor = smoothness * (sqrt(smoothness) + roughness);
    return normalize(lerp(normal, reflection, lerpFactor));
}

/**
 * Calculate a sampled direction for the Lambert BRDF.
 * @note All calculations are done in the surfaces local tangent space
 * @param albedo  The diffuse colour term.
 * @param samples Random number samples used to sample BRDF.
 * @return The sampled direction in local space.
 */
float3 SampleLambert(float3 albedo, float2 samples)
{
    // Sample the local space uniform hemisphere
    return SampleHemisphere(samples);
}

/**
 * Calculate the PDF for given values for the Lambert BRDF.
 * @param dotNL The dot product of the local normal and view direction (range [-1, 1]).
 * @return The calculated PDF.
 */
float SampleLambertPDF(float dotNL)
{
    return saturate(dotNL) / PI; // PDF for the upper hemisphere.
}

/**
 * Calculates the probability of selecting the specular component over the diffuse component of a BRDF.
 * @param F0     The fresnel reflectance at grazing angle.
 * @param dotHV  The dot product of the half-vector and view direction (range [-1, 1]).
 * @param albedo The diffuse colour term.
 * @return The probability of selecting the specular direction.
 */
float CalculateBRDFProbability(float3 F0, float dotHV, float3 albedo)
{
#ifndef DISABLE_SPECULAR_MATERIALS
    // To determine if we are sampling the diffuse or the specular component of the BRDF we need a way to
    //    weight each components contributions. To do this we use a Fresnel blend using the diffuseCompensation
    //    for the diffuse component
    float3 f = Fresnel(F0, dotHV);

    // Approximate the contribution of each component using the fresnel blend
    float specular = RadianceToLuminance(f);
    float diffuse = RadianceToLuminance(albedo * DiffuseCompensationTerm(f, dotHV));

    // Calculate probability of selecting specular component over the diffuse
    float probability = saturate(specular / max(FLT_EPSILON, specular + diffuse));
#else
    float probability = 0.0f;
#endif
    return probability;
}

#endif // SCATTERING_HLSL