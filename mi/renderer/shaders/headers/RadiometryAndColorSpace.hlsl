#ifndef RADIOMETRY_AND_COLOR_SPACE_HLSL
#define RADIOMETRY_AND_COLOR_SPACE_HLSL

// Map linear color to radiance (simple exponential heuristic)
float3 LinearColorToRadiance (float3 Color, float Gamma = 2.2f) {
    return pow(Color, Gamma);
}

// Map radiance to linear color (simple exponential heuristic)
float3 RadianceToLinearColor (float3 Radiance, float Gamma = 2.2f) {
    return pow(Radiance, 1.0f / Gamma);
}

// Map linear color to sRGB color space
float3 LinearColorToSRGBColor(float3 LinearColor) {
    float3 x = max(LinearColor, 0.0f);
    float3 SRGBLo = x * 12.92f;
    float3 SRGBHi = 1.055f * pow(x, 1.0f / 2.4f) - 0.055f;
    float3 SRGB = lerp(SRGBLo, SRGBHi, step(0.0031308f, x));
    return saturate(SRGB);
}

// Map sRGB color space to linear color
float3 SRGBColorToLinearColor(float3 SRGBColor) {
    float3 c = saturate(SRGBColor);
    float3 LinearLo = c / 12.92f;
    float3 LinearHi = pow((c + 0.055f) / 1.055f, 2.4f);
    float3 LinearColor = lerp(LinearLo, LinearHi, step(0.04045f, c));
    return LinearColor;
}

// Calculate luminance from a color
// Note: color is not radiance. Color is tone-mapped value from radiance.
float LinearColorToLuminance (float3 Color) {
    return dot(Color, float3(0.2126f, 0.7152f, 0.0722f));
}

float RadianceToLuminance (float3 Radiance, float Gamma = 2.2f) {
    return LinearColorToLuminance(RadianceToLinearColor(Radiance, Gamma));
}

float3 RGBToYCoCg(float3 c) {
    float Y  = dot(c, float3(0.25, 0.5, 0.25));
    float Co = c.r - c.b;
    float Cg = c.g - Y;
    return float3(Y, Co, Cg);
}
float3 YCoCgToRGB(float3 ycg) {
    float Y = ycg.x;
    float Co = ycg.y;
    float Cg = ycg.z;
    float r = Y + 0.5f * Co - 0.25f * Cg;
    float g = Y + Cg;
    float b = Y - 0.5f * Co - 0.25f * Cg;
    return float3(r, g, b);
}


#endif