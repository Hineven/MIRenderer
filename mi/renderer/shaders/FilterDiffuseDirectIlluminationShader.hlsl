#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "resources/CommonSamplerResources.hlsl"

struct FilterDiffuseDirectIlluminationUB {
	float POISSON_SAMPLES[8];
};

// Similar to Lumen
float GetFilterPositionWeight(float NeighborLinearDepth, float LinearDepth, float Factor = 2000.f)
{
	float DepthDifference = abs(NeighborLinearDepth - LinearDepth);
	float RelativeDepthDifference = DepthDifference / LinearDepth;
	return exp2(-Factor * (RelativeDepthDifference * RelativeDepthDifference));
}

float NormalWeight (float3 NormalA, float3 NormalB, float Factor = 4) {
	return pow(saturate(dot(NormalA, NormalB)), Factor);
}

float DilatedConvolutionKernelWeight (float2 Offset, float InvRadius, float InvRadius2) {
	// Modified Gaussian kernel (radius is the standard deviation)
	return (1.f / sqrt(TWO_PI)) * InvRadius * exp(-dot(Offset, Offset) * (0.5f * InvRadius2));
}

#define FILTER_RADIUS 1

static const float3 POISSON_8[8] =
{
    float3( -0.4706069, -0.4427112, +0.6461146 ),
    float3( -0.9057375, +0.3003471, +0.9542373 ),
    float3( -0.3487388, +0.4037880, +0.5335386 ),
    float3( +0.1023042, +0.6439373, +0.6520134 ),
    float3( +0.5699277, +0.3513750, +0.6695386 ),
    float3( +0.2939128, -0.1131226, +0.3149309 ),
    float3( +0.7836658, -0.4208784, +0.8895339 ),
    float3( +0.1564120, -0.8198990, +0.8346850 )
};

bool IsOutOfFilm(int2 PixelCoords) {
	CameraParameters C = GetActiveCamera();
	return any(PixelCoords < 0) || any(PixelCoords >= C.FilmDimensions);
}

bool IsOutOfFilm(float2 PixelFilmPosition) {
	CameraParameters C = GetActiveCamera();
	return any(PixelFilmPosition <= 0) || any(PixelFilmPosition >= C.FilmDimensions);
}

Texture2D<float4> G_Normal;
Texture2D<float> G_Depth;

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void SpatialFilterDirectIllumination (uint2 DispatchID : SV_DispatchThreadID) {
	CameraParameters C = GetActiveCamera();
	if(IsOutOfFilm(DispatchID)) return; // Out of film
	int2 CenterPixelCoords = int2(DispatchID);
	float2 CenterFilmPosition = float2(CenterPixelCoords) + 0.5f;
	float2 CenterUV = CenterFilmPosition * C.InvFilmDimensions;
	float CenterReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, CenterUV, 0).r;
	if(CenterReversedZDepth == 0) return ; // Empty pixel

	float KernelRadius = 2.8;
	
	float3 CenterNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0) - 1.f);
	float3 CenterLinearDepth = ReversedZDepthToLinearDepth(C, CenterReversedZDepth);
	float3 SumRadiance = 0;
	float SumWeight = 0;

	for(int SampleIndex = 0; SampleIndex < 8; SampleIndex++) {
		float2 FilmOffset = POISSON_8[SampleIndex].xy * KernelRadius;
		float2 SampleFilmPosition = CenterFilmPosition + FilmOffset;
		if(IsOutOfFilm(SampleFilmPosition)) continue;
		float SampleUV = SampleFilmPosition * C.InvFilmDimensions;
		float SampleReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, SampleUV, 0).r;
		if(SampleReversedZDepth == 0) continue; // Empty pixel
		float3 NeighborNormal = normalize(2.f * GetNormalTexture(C).Load(int3(TexelCoords, 0)).xyz - 1.f);
		bool   bIsNeightborTransparent = NeighborLinearDepth == 0;
		float3 NeighborRadiance = Input.Load(int3(TexelCoords, 0)).rgb;
		float Weight = GetFilterPositionWeight(NeighborLinearDepth, PixelLinearDepth);
		Weight *= NormalWeight(PixelNormal, NeighborNormal);
		float GaussianKernelWeight = DilatedConvolutionKernelWeight(Offset, UB.DI_InvFilterGaussianRadius, UB.DI_InvFilterGaussianRadius2);
		Weight = min(Weight, GaussianKernelWeight);
		Weight *= bIsNeightborTransparent ? 0 : 1;

		SumRadiance += NeighborRadiance * Weight;
		SumWeight += Weight;
		
	}
#if FILTER_PASS == 0
	RWTexture2D<float4> Output = GetRWFilteredDirectIlluminationTexture(C);
#else
	RWTexture2D<float4> Output = GetRWDirectIlluminationTexture(C);
#endif
	// if(any(isnan(SumRadiance))) {
	// 	SumRadiance = 0;
	// }
	// SumWeight = max(SumWeight, 1e-4f);
	Output[PixelCoords] = float4(SumRadiance / SumWeight, SumWeight);
}