#ifndef RADIOMETRY_HLSL
#define RADIOMETRY_HLSL

// Map color to radiance (using a simple inverse gamma correction)
float3 ColorToRadiance (float3 Color, float Gamma = 2.2f) {
    return pow(Color, Gamma);
}

// Map radiance to color (using a simple gamma correction)
float3 RadianceToColor (float3 Radiance, float Gamma = 2.2f) {
    return pow(Radiance, 1.0f / Gamma);
}

// Calculate luminance from a color
// Note: color is not radiance. Color is tone-mapped value from radiance.
float ColorToLuminance (float3 Color) {
    return dot(Color, float3(0.2126f, 0.7152f, 0.0722f));
}

#endif