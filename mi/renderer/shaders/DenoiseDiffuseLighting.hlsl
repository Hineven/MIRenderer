// Modified version of RELAX denoiser from NVIDIA NRD

#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Random.hlsl"
#include "headers/MathConstants.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Math.hlsl"
#include "resources/CommonSamplerResources.hlsl"

struct DenoiseDiffuseDirectLightingUB {
	uint  Reset;
	uint  FrameIndex;
	uint  MaxHistoryLength;
	uint  MaxFastHistoryLength;
	float DepthHistoryThreshold;
	float DilatedConvolutionLuminanceSize;
	float ConvolutionNormalDifferenceWeight;
	uint  DenoiseDiffuseIndirect;
};

ConstantBuffer<DenoiseDiffuseDirectLightingUB> UB;

// Acos(x) (approximate)
// https://www.desmos.com/calculator/x6ut8ros1u
#define AcosApprox( x ) ( sqrt( 2.0f ) * sqrt( saturate( 1.0f - x ) ) )

// Similar to Lumen
float GetFilterPositionWeight(float NeighborLinearDepth, float LinearDepth, float Factor = 2000.f)
{
	float DepthDifference = abs(NeighborLinearDepth - LinearDepth);
	float RelativeDepthDifference = DepthDifference / LinearDepth;
	return exp2(-Factor * (RelativeDepthDifference * RelativeDepthDifference));
}

float GetPlaneDistanceWeight(float3 WorldPosition, float3 CenterWorldPosition, float3 Normal, float RefLinearDepth, float Factor = 2000.f) {
	float Distance = dot(WorldPosition - CenterWorldPosition, Normal);
	float RelativeDepthDifference = Distance / RefLinearDepth;
	return exp2(-Factor * (RelativeDepthDifference * RelativeDepthDifference));
}

float GetDepthWeight (float Depth1, float Depth2, float Factor = 2000.f) {
	float Distance = abs(Depth1 - Depth2);
	float RelativeDepthDifference = Distance / max(min(Depth1, Depth2), 1e-5f);
	return exp2(-Factor * (RelativeDepthDifference * RelativeDepthDifference));
}

float NormalWeight (float3 NormalA, float3 NormalB, float Relaxtion = 0) {
	float Theta = AcosApprox(saturate(dot(NormalA, NormalB)));
	return exp2(- (1.5 - saturate(Relaxtion)) * Theta * UB.ConvolutionNormalDifferenceWeight);
}

float GaussianWeight (float r) { // Assume sigma = 1
	return exp( -0.66 * r * r );
}

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
	return any(PixelFilmPosition < 0) || any(PixelFilmPosition >= C.FilmDimensions);
}

Texture2D<float4> G_Normal;
Texture2D<float> G_Depth;

Texture2D<float>  PreviousDepthTexture;

SamplerState PointBorder0Sampler;

Texture2D<float4> InputDiffuseDirectRadianceTexture;
Texture2D<float4> InputDiffuseIndirectRadianceTexture;
Texture2D<float> PreviousHistoryLengthTexture;

[[vk::image_format("r8")]]
RWTexture2D<float> RWHistoryLengthTexture;

Texture2D<float4> PreviousPreFilteredDiffuseDirectRadianceTexture;
Texture2D<float4> PreviousDenoisedDiffuseIndirectRadianceTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWPreFilteredDiffuseDirectRadianceTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDenoisedDiffuseDirectRadianceTexture;


[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDenoisedDiffuseIndirectRadianceTexture;

#define ANTILAG_ACCELERATION_AMOUNT_SCALE 0.4f

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

// Keep how many current frame pixels reprojected to the same previous frame pixel
RWTexture2D<uint> RWPreviousFrameShareCountTexture;
// Keep the minimum depth among the current frame pixels reprojected to the same previous frame pixel
// That pixel with minimum depth is most likely to be the reasonable predecessor for the previous frame pixel
RWTexture2D<uint> RWPreviousFrameShareMinDepthTexture;

uint PackShareMinDepthRecord (float Depth, uint PixelMark) {
	uint DepthAsUint = asuint(Depth);
	return (DepthAsUint & 0xffffffc0u) | ((PixelMark & 0x3fu));
}
uint UnpackPixelMarkFromShareMinDepthRecord (uint Record) {
	return Record & 0x3fu;
}

uint GetPixelMark (uint2 Pixel) {
	return (Pixel.x & 0xfu) ^ ((Pixel.y & 0xfu) << 2);
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ClearPreviousFrameShareTextures (uint2 DispatchID : SV_DispatchThreadID) {
	CameraParameters C = GetActiveCamera();
	if(IsOutOfFilm(DispatchID)) return; // Out of film
	RWPreviousFrameShareCountTexture[DispatchID] = 0;
	RWPreviousFrameShareMinDepthTexture[DispatchID] = PackShareMinDepthRecord(FLT_MAX, 0);
}

float GetGaussianDistributionSimilarityWeight (float mu1, float sigma1, float mu2, float sigma2) {
	// Bhattacharyya distance
	float s2 = max(sigma1 * sigma1 + sigma2 * sigma2, 1e-6f);
	return sqrt(2 * sigma1 * sigma2 / s2) * exp( - Squared(mu1 - mu2) / (4 * s2) );
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void PreFilterDiffuseLightingAndTemporalAccumulate (uint2 DispatchID : SV_DispatchThreadID) {
	CameraParameters C = GetActiveCamera();
	CameraParameters PrevC = GetPreviousCamera();
	if(IsOutOfFilm(DispatchID)) return; // Out of film
	uint2 CenterPixelCoords = int2(DispatchID);
	float2 CenterFilmPosition = float2(CenterPixelCoords) + 0.5f;
	float2 CenterUV = CenterFilmPosition * C.InvFilmDimensions;
	float  CenterReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, CenterUV, 0).r;
	float  ToLinear = 1.f / length(NDC2ToCameraDirectionUnnormalized(C, UVToNDC2(CenterUV)));
	bool bSurface = true;
	if(CenterReversedZDepth == 0) {
		bSurface = false;
		RWPreFilteredDiffuseDirectRadianceTexture[CenterPixelCoords] = 0.f.xxxx;
		RWDenoisedDiffuseIndirectRadianceTexture[CenterPixelCoords] = 0.f.xxxx;
		RWHistoryLengthTexture[CenterPixelCoords] = 0;
	}
	if(!bSurface) {
		// No lighting in this pixel. Stop processing.
		return ;
	}

	// Prefilter input diffuse lighting
	float KernelRadius = 2.8;
	float3 CenterNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0).xyz - 1.f);
	float  CenterLinearDepth = ReversedZDepthToLinearDepth(C, CenterReversedZDepth);
	float3 CenterWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(CenterUV), CenterLinearDepth);
	float3 SumDiffuseDirectRadiance = 0;
	float3 SumDiffuseIndirectRadiance = 0;
	float SumDiffusePreFilterWeight = 0;

	for(int SampleIndex = 0; SampleIndex < 8; SampleIndex++) {
		float3 PoissionSample = POISSON_8[SampleIndex];
		float RotationAngle = InterleavedGradientNoise(3761.f.xx + CenterFilmPosition, UB.FrameIndex) * (2 * PI);
		PoissionSample.xy = RotateVector2D(PoissionSample.xy, RotationAngle);
		float2 FilmOffset = PoissionSample.xy * KernelRadius;
		float2 SampleFilmPosition = CenterFilmPosition + FilmOffset;
		float2 SampleUV = SampleFilmPosition * C.InvFilmDimensions;
		float SampleToLinear = 1.f / length(NDC2ToCameraDirectionUnnormalized(C, UVToNDC2(SampleUV)));
		// Mesh Diffuse
		if(bSurface) {
			float SampleReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, SampleUV, 0).r;
			float3 SampleNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, SampleUV, 0).xyz - 1.f);
			float3 SampleDiffuseDirectRadiance = InputDiffuseDirectRadianceTexture.SampleLevel(PointEdgeSampler, SampleUV, 0).rgb;
			float3 SampleDiffuseIndirectRadiance = InputDiffuseIndirectRadianceTexture.SampleLevel(PointEdgeSampler, SampleUV, 0).rgb;
			float SampleLinearDepth = ReversedZDepthToLinearDepth(C, SampleReversedZDepth);
			float3 SampleWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(SampleUV), SampleLinearDepth);
			float Weight = GetPlaneDistanceWeight(SampleWorldPosition, CenterWorldPosition, CenterNormal, CenterLinearDepth);
			Weight *= NormalWeight(CenterNormal, SampleNormal, 1);
			Weight *= GaussianWeight(PoissionSample.z);
			if(IsOutOfFilm(SampleFilmPosition) || SampleReversedZDepth == 0) { // Empty pixel
				Weight *= 0;
			}
			SumDiffuseDirectRadiance   += SampleDiffuseDirectRadiance   * Weight;
			SumDiffuseIndirectRadiance += SampleDiffuseIndirectRadiance * Weight;
			SumDiffusePreFilterWeight  += Weight;
		}
	}
	float3 NewDiffuseDirectRadiance = SumDiffuseDirectRadiance / max(SumDiffusePreFilterWeight, 1e-5f);
	float  NewDiffuseDirectLuminance = RadianceToLuminance(NewDiffuseDirectRadiance);
	float4 NewDiffuseDirectRadianceVariance = float4(NewDiffuseDirectRadiance, NewDiffuseDirectLuminance * NewDiffuseDirectLuminance);
	
	// Fetch history data
	float4 OldDiffuseDirectRadianceVariance = 0;
	float4 OldDiffuseIndirectRadianceVariance = 0;
	float OldHistoryLength = 0;
	// Reproject from previous frame to get history information
	if(bSurface) {
		float3 CenterNDC = float3(UVToNDC2(CenterUV), 1 - CenterReversedZDepth);
		float3 PreviousNDC  = ReprojectToPreviousNDCFromNDC(C, CenterNDC);
		float3 ViewDirection = -NDC2ToCameraDirection(C, CenterNDC.xy);
		// TODO: the normal should be a low frequency one.
		float3 Normal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0).xyz - 1.f);
		
		if(all(PreviousNDC.xy >= -1) && all(PreviousNDC.xy <= 1)) {
			float2 PreviousScreenPosition = NDC2ToScreenPosition(PrevC, PreviousNDC.xy);
			// Make sure the billinear weights are consistent with gathered texels
			float2 HistoryBillinearPos = PreviousScreenPosition - 0.5f;
			float2 HistoryBillinearSubPixel = frac(HistoryBillinearPos);
			float2 HistoryBillinearPixel = floor(HistoryBillinearPos);
			// Use previous camera film dimensions for previous frame gather
			float2 HistoryGatherUV = (HistoryBillinearPixel + 1.f) * PrevC.InvFilmDimensions;
			float4 HistoryZDepths = 1 - PreviousDepthTexture.GatherRed(PointBorder0Sampler, HistoryGatherUV).wzxy;
			float  PrevLinearDepth = ZDepthToLinearDepth(PrevC, PreviousNDC.z);
			float4 HistoryLinearDepths = float4(
				ZDepthToLinearDepth(C, HistoryZDepths.x),
				ZDepthToLinearDepth(C, HistoryZDepths.y),
				ZDepthToLinearDepth(C, HistoryZDepths.z),
				ZDepthToLinearDepth(C, HistoryZDepths.w)
			);
			float4 DepthDistances = abs(HistoryLinearDepths - PrevLinearDepth);
			float  Noise = InterleavedGradientNoise(CenterFilmPosition, UB.FrameIndex);
			float4 DepthThresholds = UB.DepthHistoryThreshold * lerp(0.5, 1.5, Noise);
			// UE's solution to false-positive disocclusions on smooth geometry edges
			DepthThresholds /= clamp(saturate(dot(ViewDirection, Normal)), .1f, 1.0f); 
			float4 OcclusionWeights   = select(DepthDistances < PrevLinearDepth * DepthThresholds, 1, 0);
			float4 BillinearWeights = float4(
				(1.f - HistoryBillinearSubPixel.x) * (1.f - HistoryBillinearSubPixel.y),
				HistoryBillinearSubPixel.x * (1.f - HistoryBillinearSubPixel.y),
				(1.f - HistoryBillinearSubPixel.x) * HistoryBillinearSubPixel.y,
				HistoryBillinearSubPixel.x * HistoryBillinearSubPixel.y
			);
			float4 Weights = OcclusionWeights * BillinearWeights;
			Weights = Weights / max(dot(Weights, 1), 1e-6f);

			Texture2D<float4> HistoryDD = PreviousPreFilteredDiffuseDirectRadianceTexture;
			{
				int2 BasePixel = HistoryBillinearPixel;
				int2 P00 = clamp(BasePixel, int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P10 = clamp(BasePixel + int2(1, 0), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P01 = clamp(BasePixel + int2(0, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P11 = clamp(BasePixel + int2(1, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				float4 R00 = HistoryDD.Load(int3(P00, 0));
				float4 R10 = HistoryDD.Load(int3(P10, 0));
				float4 R01 = HistoryDD.Load(int3(P01, 0));
				float4 R11 = HistoryDD.Load(int3(P11, 0));
				OldDiffuseDirectRadianceVariance = R00 * Weights.x + R10 * Weights.y + R01 * Weights.z + R11 * Weights.w;
			}
			Texture2D<float4> HistoryDI = PreviousDenoisedDiffuseIndirectRadianceTexture;
			{
				int2 BasePixel = HistoryBillinearPixel;
				int2 P00 = clamp(BasePixel, int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P10 = clamp(BasePixel + int2(1, 0), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P01 = clamp(BasePixel + int2(0, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P11 = clamp(BasePixel + int2(1, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				float4 R00 = HistoryDI.Load(int3(P00, 0));
				float4 R10 = HistoryDI.Load(int3(P10, 0));
				float4 R01 = HistoryDI.Load(int3(P01, 0));
				float4 R11 = HistoryDI.Load(int3(P11, 0));
				OldDiffuseIndirectRadianceVariance = R00 * Weights.x + R10 * Weights.y + R01 * Weights.z + R11 * Weights.w;
			}
			Texture2D<float> HistoryLength = PreviousHistoryLengthTexture;
			{
				int2 BasePixel = HistoryBillinearPixel;
				int2 P00 = clamp(BasePixel, int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P10 = clamp(BasePixel + int2(1, 0), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P01 = clamp(BasePixel + int2(0, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P11 = clamp(BasePixel + int2(1, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				float R00 = HistoryLength.Load(int3(P00, 0)).r;
				float R10 = HistoryLength.Load(int3(P10, 0)).r;
				float R01 = HistoryLength.Load(int3(P01, 0)).r;
				float R11 = HistoryLength.Load(int3(P11, 0)).r;
				OldHistoryLength = dot(float4(R00, R10, R01, R11), Weights) * 255.f;
			}
		}
	}


	float NewHistoryLength = min(OldHistoryLength + 1, UB.MaxHistoryLength);


	float OutAlpha = 1 / NewHistoryLength;
// 	float OutFastAlpha = 1 / max(NewFastHistoryLength, 1);

	// Update fast history
	// float3 OldFastRadiance = RWPreFilteredFastRadianceTexture[CenterPixelCoords].xyz;
	// float3 OutFastRadiance = lerp(OldFastRadiance, NewDiffuseDirectRadianceVariance.rgb, OutFastAlpha);

	// Update history
	float4 OutDiffuseDirectRadianceVariance = float4(
		lerp(OldDiffuseDirectRadianceVariance.rgb, NewDiffuseDirectRadianceVariance.rgb, OutAlpha),
		NewDiffuseDirectRadianceVariance.a // Use newest filtered luminance variance (no blending)
	);
	OutDiffuseDirectRadianceVariance = clamp(OutDiffuseDirectRadianceVariance, 0, FP16_MAX);
	float4 OutDiffuseIndirectRadianceVariance = float4(
		lerp(OldDiffuseIndirectRadianceVariance.rgb, SumDiffuseIndirectRadiance / max(SumDiffusePreFilterWeight, 1e-5f), OutAlpha),
		0
	);
	OutDiffuseIndirectRadianceVariance = clamp(OutDiffuseIndirectRadianceVariance, 0, FP16_MAX);
	
	// Reset history if requested
	if(UB.Reset == 1) {
		OutDiffuseDirectRadianceVariance = NewDiffuseDirectRadianceVariance;
		OutDiffuseIndirectRadianceVariance = float4(SumDiffuseIndirectRadiance / max(SumDiffusePreFilterWeight, 1e-5f), 0);
		NewHistoryLength = bSurface;
	}

	// TODO history acceleration

	// Write out
    if(bSurface) {
        RWPreFilteredDiffuseDirectRadianceTexture[CenterPixelCoords] = OutDiffuseDirectRadianceVariance;
#ifdef OUTPUT_DIRECTLY
        RWDenoisedDiffuseDirectRadianceTexture[CenterPixelCoords] = float4(OutDiffuseDirectRadianceVariance.rgb, NewHistoryLength);
#endif
        if(UB.DenoiseDiffuseIndirect) {
            RWDenoisedDiffuseIndirectRadianceTexture[CenterPixelCoords] = float4(OutDiffuseIndirectRadianceVariance.rgb, 1);
        } else {
            float3 OriginalDiffuseIndirect = InputDiffuseIndirectRadianceTexture.SampleLevel(PointEdgeSampler, CenterUV, 0).rgb;
            RWDenoisedDiffuseIndirectRadianceTexture[CenterPixelCoords] = float4(OriginalDiffuseIndirect, 1);
        }
        RWHistoryLengthTexture[CenterPixelCoords] = NewHistoryLength / 255.f;
    }
}


float GetNormalWeightRelaxation (float Fraction) {
	return saturate(Fraction * Fraction);
}

Texture2D<float> HistoryLengthTexture;
Texture2D<float4> DilatedFilterInputDiffuseDirectRadianceTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDilatedFilterOutputFilteredDiffuseDirectRadiance;

#ifndef DILATED_FILTER_PASS
#define DILATED_FILTER_PASS 0
#endif

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void DilatedFilterDiffuseDirectLighting (uint2 DispatchID : SV_DispatchThreadID) {
	CameraParameters C = GetActiveCamera();
	if(IsOutOfFilm(DispatchID)) return; // Out of film
	uint2 CenterPixelCoords = DispatchID;
	float2 CenterFilmPosition = float2(CenterPixelCoords) + 0.5f;
	float2 CenterUV = CenterFilmPosition * C.InvFilmDimensions;
	float CenterReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, CenterUV, 0).r;
	float ToLinear = 1.f / length(NDC2ToCameraDirectionUnnormalized(C, UVToNDC2(CenterUV)));
	bool bSurface = CenterReversedZDepth != 0;
	if(!bSurface) {
		// Empty pixel
		RWDilatedFilterOutputFilteredDiffuseDirectRadiance[CenterPixelCoords] = float4(0, 0, 0, 0);
	}
	// Early out if the pixel is empty
	[branch]
	if(!bSurface) {
		return ;
	}

	float DilatedStepSize = 1 << (DILATED_FILTER_PASS + 1);

    static const float KernelWeightGaussian3x3[2] = { 0.44198, 0.27901 };

	float  HistoryLength = HistoryLengthTexture.SampleLevel(PointEdgeSampler, CenterUV, 0).r * 255;
	
	float3 CenterNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0).xyz - 1.f);
	float  CenterLinearDepth = ReversedZDepthToLinearDepth(C, CenterReversedZDepth);
	float3 CenterWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(CenterUV), CenterLinearDepth);
	
	float4 CenterRadianceVariance = DilatedFilterInputDiffuseDirectRadianceTexture.SampleLevel(PointEdgeSampler, CenterUV, 0);

    // Diffuse normal weight is used for diffuse and can be used for specular depending on settings.
    // Weight strictness is higher as the Atrous step size increases.
    float PixelLobeAngleFraction = 1.f / sqrt(DilatedStepSize);
	// Relax lobe normal requirements for low history pixels for fast converngence
    PixelLobeAngleFraction = lerp(0.99, PixelLobeAngleFraction, saturate(HistoryLength / 5.0));

	float4 SumDiffuseDirectRadianceVariance = 
		float4(KernelWeightGaussian3x3[0] * CenterRadianceVariance.rgb,
		 KernelWeightGaussian3x3[0] * KernelWeightGaussian3x3[0] * CenterRadianceVariance.w * CenterRadianceVariance.w);
	float SumDiffuseDirectWeight = KernelWeightGaussian3x3[0];

	// Variance based filtering
    float CenterLuminance = RadianceToLuminance(CenterRadianceVariance.rgb);
    float CenterVariance = CenterRadianceVariance.a;
	// Compute tolerance for difference in luminance when filtering
    float CenterPhiLIlluminationInv = 1.0 / max(1.0e-4, UB.DilatedConvolutionLuminanceSize * sqrt(CenterVariance));

    float LuminanceWeightRelaxation = 1.0; // TODO make this useful

    float NormalWeightRelaxation = GetNormalWeightRelaxation(PixelLobeAngleFraction);

    [unroll]
    for (int PixelYOffset = -1; PixelYOffset <= 1; PixelYOffset++)
    {
        [unroll]
        for (int PixelXOffset = -1; PixelXOffset <= 1; PixelXOffset++)
        {
			if(PixelXOffset == 0 && PixelYOffset == 0) continue; // Already added center pixel
            float2 SampleFilmPosition = CenterFilmPosition + float2(PixelXOffset, PixelYOffset) * DilatedStepSize;
			if(IsOutOfFilm(SampleFilmPosition)) continue; // Out of film
            float2 SampleUV = SampleFilmPosition * C.InvFilmDimensions;
			float KernelWeight = KernelWeightGaussian3x3[abs(PixelXOffset)] * KernelWeightGaussian3x3[abs(PixelYOffset)];

			// Diffuse direct
			if(bSurface) {
				float3 SampleNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, SampleUV, 0).xyz - 1.f);
				float SampleReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, SampleUV, 0).r;
				float SampleLinearDepth = ReversedZDepthToLinearDepth(C, SampleReversedZDepth);

				// Calculating sample world position
				float3 SampleWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(SampleUV), SampleLinearDepth);

				// Calculating geometry weight for diffuse and specular
				float SampleWeight = GetPlaneDistanceWeight(SampleWorldPosition, CenterWorldPosition, CenterNormal, CenterLinearDepth);
				SampleWeight *= KernelWeight;

				SampleWeight *= NormalWeight(CenterNormal, SampleNormal, NormalWeightRelaxation);

				if (SampleWeight > 1e-4)
				{
					float4 SampledRadianceVariance = DilatedFilterInputDiffuseDirectRadianceTexture.SampleLevel(PointEdgeSampler, SampleUV, 0);
					float SampleLuminance = RadianceToLuminance(SampledRadianceVariance.rgb);

					// Larget the variance, more tolerance for luminace differences
					float LuminanceWeight = abs(CenterLuminance - SampleLuminance) * CenterPhiLIlluminationInv;
					LuminanceWeight = min(10.f, LuminanceWeight);
					LuminanceWeight *= LuminanceWeightRelaxation;

					SampleWeight *= exp(-LuminanceWeight);

					SumDiffuseDirectWeight += SampleWeight;
					SumDiffuseDirectRadianceVariance += float4(SampleWeight.xxx, SampleWeight * SampleWeight) * SampledRadianceVariance;
				}
			}
        }
    }

    float4 FilteredRadianceVariance = float4(SumDiffuseDirectRadianceVariance / float4(SumDiffuseDirectWeight.xxx, SumDiffuseDirectWeight * SumDiffuseDirectWeight));
#ifdef LAST_PASS
	// Write history back to the texture in the last pass for visualization purposes
	FilteredRadianceVariance.w       = HistoryLength;
#endif
	if(bSurface) RWDilatedFilterOutputFilteredDiffuseDirectRadiance[CenterPixelCoords] = FilteredRadianceVariance;
}
