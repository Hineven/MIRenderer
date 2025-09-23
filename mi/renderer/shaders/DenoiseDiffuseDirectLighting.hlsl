// Modified version of RELAX denoiser from NVIDIA NRD

#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Random.hlsl"
#include "headers/MathConstants.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/Conventions.hlsl"
#include "resources/CommonSamplerResources.hlsl"

struct DenoiseDiffuseDirectLightingUB {
	float RotationAngle;
	uint  FrameIndex;
	uint  MaxHistoryLength;
	uint  MaxFastHistoryLength;
	float DepthHistoryThreshold;
	float DilatedConvolutionLuminanceSize;
	float ConvolutionNormalDifferenceWeight;
	uint Padding;
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
	return any(PixelFilmPosition <= 0) || any(PixelFilmPosition >= C.FilmDimensions);
}

Texture2D<float4> G_Normal;
Texture2D<float> G_Depth;

Texture2D<float4> HistoryDepthTexture;
SamplerState PointBorder0Sampler;

Texture2D<float4> OriginalRadianceTexture;
[[vk::image_format("r8")]]
RWTexture2D<float> RWHistoryLengthTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWHistoryRadianceTexture;
// RWTexture2D<float4> RWPreFilteredFastRadianceTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWPreFilteredRadianceTexture;

#define ANTILAG_ACCELERATION_AMOUNT_SCALE 0.4f

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void PreFilterDirectLightingAndTemporalAccumulate (uint2 DispatchID : SV_DispatchThreadID) {
	CameraParameters C = GetActiveCamera();
	if(IsOutOfFilm(DispatchID)) return; // Out of film
	int2 CenterPixelCoords = int2(DispatchID);
	float2 CenterFilmPosition = float2(CenterPixelCoords) + 0.5f;
	float2 CenterUV = CenterFilmPosition * C.InvFilmDimensions;
	float CenterReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, CenterUV, 0).r;
	if(CenterReversedZDepth == 0) {
		RWPreFilteredRadianceTexture[CenterPixelCoords] = float4(0, 0, 0, 0);
		// RWPreFilteredFastRadianceTexture[CenterPixelCoords] = float4(0, 0, 0, 0);
#ifndef OUTPUT_DIRECTLY
		RWHistoryLengthTexture[CenterPixelCoords] = 0;
#endif
		return ; // Empty pixel
	}

	float KernelRadius = 2.8;
	
	float3 CenterNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0).xyz - 1.f);
	float  CenterLinearDepth = ReversedZDepthToLinearDepth(C, CenterReversedZDepth);
	float3 CenterWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(CenterUV), CenterLinearDepth);
	float3 SumRadiance = 0;
	float SumWeight = 0;

	for(int SampleIndex = 0; SampleIndex < 8; SampleIndex++) {
		float3 PoissionSample = POISSON_8[SampleIndex];
		float RotationAngle = InterleavedGradientNoise(CenterUV, UB.FrameIndex) * (2 * PI);
		PoissionSample.xy = RotateVector2D(PoissionSample.xy, RotationAngle);
		float2 FilmOffset = PoissionSample.xy * KernelRadius;
		float2 SampleFilmPosition = CenterFilmPosition + FilmOffset;
		float2 SampleUV = SampleFilmPosition * C.InvFilmDimensions;
		float SampleReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, SampleUV, 0).r;
		float3 SampleNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, SampleUV, 0).xyz - 1.f);
		float3 SampleRadiance = OriginalRadianceTexture.SampleLevel(PointEdgeSampler, SampleUV, 0).rgb;
		float SampleLinearDepth = ReversedZDepthToLinearDepth(C, SampleReversedZDepth);
		float3 SampleWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(SampleUV), SampleLinearDepth);
		float Weight = GetPlaneDistanceWeight(SampleWorldPosition, CenterWorldPosition, CenterNormal, CenterLinearDepth);
		Weight *= NormalWeight(CenterNormal, SampleNormal, 1);
		Weight *= GaussianWeight(PoissionSample.z);
		if(IsOutOfFilm(SampleFilmPosition) || SampleReversedZDepth == 0) { // Empty pixel
			Weight *= 0;
		}
		SumRadiance += SampleRadiance * Weight;
		SumWeight += Weight;
	}
	float3 NewRadiance = SumRadiance / SumWeight;
	float NewLuminance = RadianceToLuminance(NewRadiance);
	float4 NewRadianceVariance = float4(NewRadiance, NewLuminance * NewLuminance);
	
	float OldRadianceHistoryLength = 0;
	float3 OldRadiance = 0;
	// Reproject from previous frame to get history information
	{
		float3 CenterNDC = float3(UVToNDC2(CenterUV), 1 - CenterReversedZDepth);
		float3 PreviousNDC  = ReprojectToPreviousNDCFromNDC(C, CenterNDC);
		float3 ViewDirection = -NDC2ToCameraDirection(C, CenterNDC.xy);
		// TODO: the normal should be a low frequency one.
		float3 Normal = normalize(G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0).xyz - 0.5);
		
		if(all(PreviousNDC.xy >= -1) && all(PreviousNDC.xy <= 1)) {
			float2 PreviousScreenPosition = NDC2ToScreenPosition(C, PreviousNDC.xy);
			// Make sure the billinear weights are consistent with gathered texels
			float2 HistoryBillinearPos = PreviousScreenPosition - 0.5f;
			float2 HistoryBillinearSubPixel = frac(HistoryBillinearPos);
			float2 HistoryBillinearPixel = floor(HistoryBillinearPos);
			float2 HistoryGatherUV = (HistoryBillinearPixel + 1.f) * C.InvFilmDimensions;
			float4 HistoryZDepths = 1 - HistoryDepthTexture.GatherRed(PointBorder0Sampler, HistoryGatherUV).wzxy;
			float  PrevLinearDepth = ZDepthToLinearDepth(C, PreviousNDC.z);
			float4 HistoryLinearDepths = float4(
				ZDepthToLinearDepth(C, HistoryZDepths.x),
				ZDepthToLinearDepth(C, HistoryZDepths.y),
				ZDepthToLinearDepth(C, HistoryZDepths.z),
				ZDepthToLinearDepth(C, HistoryZDepths.w)
			);
			float4 DepthDistances = abs(HistoryLinearDepths - PrevLinearDepth);
			float  Noise = InterleavedGradientNoise(NDC2ToUV(CenterNDC.xy), UB.FrameIndex);
			float4 DepthThresholds = UB.DepthHistoryThreshold * lerp(0.5, 1.5, Noise);
			// UE's resolution for disocclusion misses on geometry edges
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

			float4 OldData = 0.f;
			RWTexture2D<float4> HistoryDI = RWHistoryRadianceTexture;
			{
				int2 BasePixel = HistoryBillinearPixel;
				int2 P00 = clamp(BasePixel, int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P10 = clamp(BasePixel + int2(1, 0), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P01 = clamp(BasePixel + int2(0, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				int2 P11 = clamp(BasePixel + int2(1, 1), int2(0, 0), int2(C.FilmDimensions - 1));
				float4 R00 = HistoryDI[P00];
				float4 R10 = HistoryDI[P10];
				float4 R01 = HistoryDI[P01];
				float4 R11 = HistoryDI[P11];
				OldData = R00 * Weights.x + R10 * Weights.y + R01 * Weights.z + R11 * Weights.w;
			}
			OldRadiance = OldData.xyz;
			OldRadianceHistoryLength = OldData.w;
		}
	}

	float OldHistoryLength = OldRadianceHistoryLength;
	float NewHistoryLength = min(OldHistoryLength + 1, UB.MaxHistoryLength);
	float NewFastHistoryLength = min(OldHistoryLength, UB.MaxFastHistoryLength);

	float OutAlpha = 1 / NewHistoryLength;
	float OutFastAlpha = 1 / max(NewFastHistoryLength, 1);

	// Update fast history
	// float3 OldFastRadiance = RWPreFilteredFastRadianceTexture[CenterPixelCoords].xyz;
	// float3 OutFastRadiance = lerp(OldFastRadiance, NewRadianceVariance.rgb, OutFastAlpha);

	// Update history
	float4 OutRadianceVariance = float4(lerp(OldRadiance, NewRadianceVariance.rgb, OutAlpha), NewRadianceVariance.a);
	OutRadianceVariance = clamp(OutRadianceVariance, 0, FP16_MAX);

	// TODO history acceleration

	// Write out
#ifndef OUTPUT_DIRECTLY
	RWPreFilteredRadianceTexture[CenterPixelCoords] = OutRadianceVariance;
	// RWPreFilteredFastRadianceTexture[CenterPixelCoords] = float4(OutFastRadiance, 0);
	RWHistoryLengthTexture[CenterPixelCoords] = NewHistoryLength / 255.f;
#else
	RWPreFilteredRadianceTexture[CenterPixelCoords] = float4(OutRadianceVariance.rgb, NewHistoryLength);
#endif
}


float GetNormalWeightRelaxation (float Fraction) {
	return saturate(Fraction * Fraction);
}

Texture2D<float> HistoryLengthTexture;
Texture2D<float4> DilatedFilterInputRadianceTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDilatedFilterOutputFilteredRadiance;

#ifndef DILATED_FILTER_PASS
#define DILATED_FILTER_PASS 0
#endif

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void DilatedFilterDirectLighting (uint2 DispatchID : SV_DispatchThreadID) {
	CameraParameters C = GetActiveCamera();
	if(IsOutOfFilm(DispatchID)) return; // Out of film
	int2 CenterPixelCoords = int2(DispatchID);
	float2 CenterFilmPosition = float2(CenterPixelCoords) + 0.5f;
	float2 CenterUV = CenterFilmPosition * C.InvFilmDimensions;
	float CenterReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, CenterUV, 0).r;
	if(CenterReversedZDepth == 0) {
		RWDilatedFilterOutputFilteredRadiance[CenterPixelCoords] = float4(0, 0, 0, 0);
		return ; // Empty pixel
	}

	float DilatedStepSize = 1 << (DILATED_FILTER_PASS + 1);

    static const float KernelWeightGaussian3x3[2] = { 0.44198, 0.27901 };

	
	float3 CenterNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, CenterUV, 0).xyz - 1.f);
	float  CenterLinearDepth = ReversedZDepthToLinearDepth(C, CenterReversedZDepth);
	float3 CenterWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(CenterUV), CenterLinearDepth);
	
	float4 CenterRadianceVariance = DilatedFilterInputRadianceTexture.SampleLevel(PointEdgeSampler, CenterUV, 0);
	float  HistoryLength = HistoryLengthTexture.SampleLevel(PointEdgeSampler, CenterUV, 0).r * 255;

    // Diffuse normal weight is used for diffuse and can be used for specular depending on settings.
    // Weight strictness is higher as the Atrous step size increases.
    float PixelLobeAngleFraction = 1.f / sqrt(DilatedStepSize);
	// Relax lobe normal requirements for low history pixels for fast converngence
    PixelLobeAngleFraction = lerp(0.99, PixelLobeAngleFraction, saturate(HistoryLength / 5.0));

	float4 SumRadianceVariance = 
		float4(KernelWeightGaussian3x3[0] * CenterRadianceVariance.rgb,
		 KernelWeightGaussian3x3[0] * KernelWeightGaussian3x3[0] * CenterRadianceVariance.w * CenterRadianceVariance.w);
	float SumWeight = KernelWeightGaussian3x3[0];

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
			float GaussianWeight = KernelWeightGaussian3x3[abs(PixelXOffset)] * KernelWeightGaussian3x3[abs(PixelYOffset)];


            float3 SampleNormal = normalize(2.f * G_Normal.SampleLevel(PointEdgeSampler, SampleUV, 0).xyz - 1.f);
            float SampleReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, SampleUV, 0).r;
			float SampleLinearDepth = ReversedZDepthToLinearDepth(C, SampleReversedZDepth);

            // Calculating sample world position
			float3 SampleWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(SampleUV), SampleLinearDepth);

            // Calculating geometry weight for diffuse and specular
            float SampleWeight = GetPlaneDistanceWeight(SampleWorldPosition, CenterWorldPosition, CenterNormal, CenterLinearDepth);
            SampleWeight *= GaussianWeight;

            SampleWeight *= NormalWeight(CenterNormal, SampleNormal, NormalWeightRelaxation);

            if (SampleWeight > 1e-4)
            {
                float4 SampledRadianceVariance = DilatedFilterInputRadianceTexture.SampleLevel(PointEdgeSampler, SampleUV, 0);
                float SampleLuminance = RadianceToLuminance(SampledRadianceVariance.rgb);

				// Larget the variance, more tolerance for luminace differences
                float LuminanceWeight = abs(CenterLuminance - SampleLuminance) * CenterPhiLIlluminationInv;
                LuminanceWeight = min(10.f, LuminanceWeight);
                LuminanceWeight *= LuminanceWeightRelaxation;

                SampleWeight *= exp(-LuminanceWeight);

                SumWeight += SampleWeight;
                SumRadianceVariance +=  float4(SampleWeight.xxx, SampleWeight * SampleWeight) * SampledRadianceVariance;
            }

        }
    }


    float4 FilteredRadianceVariance = float4(SumRadianceVariance / float4(SumWeight.xxx, SumWeight * SumWeight));
#ifdef LAST_PASS
	// Write history back to the texture in the last pass for future reuse
	FilteredRadianceVariance.w = HistoryLength;
#endif
    RWDilatedFilterOutputFilteredRadiance[CenterPixelCoords] = FilteredRadianceVariance;
}