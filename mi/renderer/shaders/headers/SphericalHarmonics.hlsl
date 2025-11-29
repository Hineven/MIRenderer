// Copy-pasted from Capsaicin

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

#ifndef SPHERICAL_HARMONICS_HLSL
#define SPHERICAL_HARMONICS_HLSL

#include "MathConstants.hlsl"

void SH_GetCoefficients(in float3 direction, out float coefficients[9])
{
    float fC0, fC1, fS0, fS1, fTmpA, fTmpB, fTmpC;
    float pz2 = direction.z * direction.z;
    coefficients[0] = 0.2820947917738781f;
    coefficients[2] = 0.4886025119029199f * direction.z;
    coefficients[6] = 0.9461746957575601f * pz2 + -0.3153915652525201f;
    fC0 = direction.x;
    fS0 = direction.y;
    fTmpA = -0.48860251190292f;
    coefficients[3] = fTmpA * fC0;
    coefficients[1] = fTmpA * fS0;
    fTmpB = -1.092548430592079f * direction.z;
    coefficients[7] = fTmpB * fC0;
    coefficients[5] = fTmpB * fS0;
    fC1 = direction.x * fC0 - direction.y * fS0;
    fS1 = direction.x * fS0 + direction.y * fC0;
    fTmpC = 0.5462742152960395f;
    coefficients[8] = fTmpC * fC1;
    coefficients[4] = fTmpC * fS1;
}

// Check Capsaicin wiki SH page for derivations
void SH_GetCoefficients_ClampedCosine(in float3 cosine_lobe_dir, out float coefficients[9])
{
    // cosine_lobe_dir is normalized
    const float CosineA0 = PI;
    const float CosineA1 = (2.0f * PI) / 3.0f;
    const float CosineA2 = PI / 4.0f;
    float fC0, fC1, fS0, fS1, fTmpA, fTmpB, fTmpC;
    float pz2 = cosine_lobe_dir.z * cosine_lobe_dir.z;
    coefficients[0] = 0.2820947917738781f * CosineA0;
    coefficients[2] = 0.4886025119029199f * cosine_lobe_dir.z * CosineA1;
    coefficients[6] = 0.7431238683011271f * pz2 + -0.2477079561003757f;
    fC0 = cosine_lobe_dir.x;
    fS0 = cosine_lobe_dir.y;
    fTmpA = -0.48860251190292f;
    coefficients[3] = fTmpA * fC0 * CosineA1;
    coefficients[1] = fTmpA * fS0 * CosineA1;
    fTmpB = -1.092548430592079f * cosine_lobe_dir.z;
    coefficients[7] = fTmpB * fC0 * CosineA2;
    coefficients[5] = fTmpB * fS0 * CosineA2;
    fC1 = cosine_lobe_dir.x * fC0 - cosine_lobe_dir.y * fS0;
    fS1 = cosine_lobe_dir.x * fS0 + cosine_lobe_dir.y * fC0;
    fTmpC = 0.5462742152960395f;
    coefficients[8] = fTmpC * fC1 * CosineA2;
    coefficients[4] = fTmpC * fS1 * CosineA2;
}

// Check Capsaicin wiki SH page for derivations
void SH_GetCoefficients_ClampedCosine_Cone(in float3 cosine_lobe_dir, in float cone_theta_max, out float coefficients[9])
{
    // cosine_lobe_dir is normalized
    // cone_theta_max is <= pi
    float sin_theta_max;
    float cos_theta_max;
    sincos(cone_theta_max, sin_theta_max, cos_theta_max);
    float sin_theta_max2 = sin_theta_max * sin_theta_max;
    float sin_theta_max3 = sin_theta_max2 * sin_theta_max;
    float cos_theta_max2 = cos_theta_max * cos_theta_max;
    float cos_theta_max3 = cos_theta_max2 * cos_theta_max;

    float band1_factor = 1.023326707946489f * (1.f - cos_theta_max3);
    float band2_factor = (4.f - 3.f * sin_theta_max3) * sin_theta_max2;

    coefficients[0] = 0.886226925452758f * sin_theta_max2;

    coefficients[1] = -band1_factor * cosine_lobe_dir.y;
    coefficients[2] = +band1_factor * cosine_lobe_dir.z;
    coefficients[3] = -band1_factor * cosine_lobe_dir.x;

    coefficients[4] = +0.8580855308097834f * band2_factor * cosine_lobe_dir.x * cosine_lobe_dir.y;
    coefficients[5] = -0.8580855308097834f * band2_factor * cosine_lobe_dir.y * cosine_lobe_dir.z;
    coefficients[6] = +0.2477079561003757f * band2_factor * (3.f * cosine_lobe_dir.z * cosine_lobe_dir.z - 1.f);
    coefficients[7] = -0.8580855308097834f * band2_factor * cosine_lobe_dir.x * cosine_lobe_dir.z;
    coefficients[8] = +0.4290427654048917f * band2_factor * (cosine_lobe_dir.x * cosine_lobe_dir.x - cosine_lobe_dir.y * cosine_lobe_dir.y);
}

void SH_GetCoefficients_ClampedCosine_Cone_Window(in float3 cosine_lobe_dir, in float cone_theta_max, in float window, out float coefficients[9])
{
    // cosine_lobe_dir is normalized
    // cone_theta_max is <= pi
    // window is >= 0 and small in practice
    float sin_theta_max;
    float cos_theta_max;
    sincos(cone_theta_max, sin_theta_max, cos_theta_max);
    float sin_theta_max2 = sin_theta_max * sin_theta_max;
    float sin_theta_max3 = sin_theta_max2 * sin_theta_max;
    float cos_theta_max2 = cos_theta_max * cos_theta_max;
    float cos_theta_max3 = cos_theta_max2 * cos_theta_max;

    coefficients[0] = 0.886226925452758f * sin_theta_max2;

    float band1_factor = 1.023326707946489f * (1.f - cos_theta_max3);
    float window1 = 1.f / (4.f * window + 1.f);
    coefficients[1] = -band1_factor * window1 * cosine_lobe_dir.y;
    coefficients[2] = +band1_factor * window1 * cosine_lobe_dir.z;
    coefficients[3] = -band1_factor * window1 * cosine_lobe_dir.x;

    float band2_factor = (4.f - 3.f * sin_theta_max3) * sin_theta_max2;
    float window2 = 1.f / (36.f * window + 1.f);
    coefficients[4] = +0.8580855308097834f * band2_factor * window2 * cosine_lobe_dir.x * cosine_lobe_dir.y;
    coefficients[5] = -0.8580855308097834f * band2_factor * window2 * cosine_lobe_dir.y * cosine_lobe_dir.z;
    coefficients[6] = +0.2477079561003757f * band2_factor * window2 * (3.f * cosine_lobe_dir.z * cosine_lobe_dir.z - 1.f);
    coefficients[7] = -0.8580855308097834f * band2_factor * window2 * cosine_lobe_dir.x * cosine_lobe_dir.z;
    coefficients[8] = +0.4290427654048917f * band2_factor * window2 * (cosine_lobe_dir.x * cosine_lobe_dir.x - cosine_lobe_dir.y * cosine_lobe_dir.y);
}

void SH_GetCoefficients_HenyeyGreenstein(in float3 direction, in float g, out float coefficients[9])
{
    float base[9];
    SH_GetCoefficients(direction, base);

    g = clamp(g, -0.99f, 0.99f);

    // l=0 (index 0)
    coefficients[0] = base[0];                       // g^0

    // l=1 (indices 1,2,3)
    float g1 = g;
    coefficients[1] = base[1] * g1;
    coefficients[2] = base[2] * g1;
    coefficients[3] = base[3] * g1;

    // l=2 (indices 4..8)
    float g2 = g * g;
    coefficients[4] = base[4] * g2;
    coefficients[5] = base[5] * g2;
    coefficients[6] = base[6] * g2;
    coefficients[7] = base[7] * g2;
    coefficients[8] = base[8] * g2;
}

struct SH3Coefficents {
    float3 Coefficients[16]; // 16x3 coefficients
};


float3 SH3Evaluate(float3 ViewDirection, SH3Coefficents SH3, int Degree = 3)
{
	const float SH_C0 = 0.28209479177387814f;
	const float SH_C1 = 0.4886025119029199f;
	const float SH_C2[] = {
		1.0925484305920792f,
		-1.0925484305920792f,
		0.31539156525252005f,
		-1.0925484305920792f,
		0.5462742152960396f
	};
	const float SH_C3[] = {
		-0.5900435899266435f,
		2.890611442640554f,
		-0.4570457994644658f,
		0.3731763325901154f,
		-0.4570457994644658f,
		1.445305721320277f,
		-0.5900435899266435f
	};
	// The SH stored in the gaussians is "flipped" compared to ordinary computer graphics
	// conventions.
	float3 NViewDirection = -ViewDirection;

	float3 result = SH_C0 * SH3.Coefficients[0];

	if(Degree >= 1) {
		float x = NViewDirection.x;
		float y = NViewDirection.y;
		float z = NViewDirection.z;
		result = result - SH_C1 * y * SH3.Coefficients[1] + SH_C1 * z * SH3.Coefficients[2] - SH_C1 * x * SH3.Coefficients[3];

		if(Degree >= 2) {
			float xx = x * x, yy = y * y, zz = z * z;
			float xy = x * y, yz = y * z, xz = x * z;
			result = result +
				SH_C2[0] * xy * SH3.Coefficients[4] +
				SH_C2[1] * yz * SH3.Coefficients[5] +
				SH_C2[2] * (2.0f * zz - xx - yy) * SH3.Coefficients[6] +
				SH_C2[3] * xz * SH3.Coefficients[7] +
				SH_C2[4] * (xx - yy) * SH3.Coefficients[8];
			if(Degree >= 3) {
				result = result +
					SH_C3[0] * y * (3.0f * xx - yy) * SH3.Coefficients[9] +
					SH_C3[1] * xy * z * SH3.Coefficients[10] +
					SH_C3[2] * y * (4.0f * zz - xx - yy) * SH3.Coefficients[11] +
					SH_C3[3] * z * (2.0f * zz - 3.0f * xx - 3.0f * yy) * SH3.Coefficients[12] +
					SH_C3[4] * x * (4.0f * zz - xx - yy) * SH3.Coefficients[13] +
					SH_C3[5] * z * (xx - yy) * SH3.Coefficients[14] +
					SH_C3[6] * x * (xx - 3.0f * yy) * SH3.Coefficients[15];
			}
		}
	}
	return result;
}

#endif // SPHERICAL_HARMONICS_HLSL
