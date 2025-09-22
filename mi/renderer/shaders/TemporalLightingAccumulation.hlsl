
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void TemporalLightingAccumulation (uint2 DispatchID : SV_DispatchThreadID) {
	CameraDescription C = GetCameraDescription();
	if(IsOutOfFilm(C, DispatchID)) return;
	int2 PixelCoords = int2(DispatchID);
	Material M = FetchMaterial(C, PixelCoords);
	if(M.Alpha < UB.OpaqueThreshold) {
		return;
	}
	float PixelZDepth = GetZDepthTexture(C).Load(int3(PixelCoords, 0)).x;
	float2 ScreenPosition = PixelCoords + 0.5f.xx;
	float3 NDC = float3(ScreenToNDC2(ScreenPosition), PixelZDepth);
	float3 PreviousNDC  = ReprojectToPreviousNDCFromNDC(C, NDC);
	float3 ViewDirection = -NDC2ToCameraDirection(C, NDC.xy);
	float4 NewDirectIllumination_C  = GetDirectIlluminationTexture(C).Load(int3(PixelCoords, 0));
	float4 NewIndirectIllumination_Diffuse_C = GetIndirectIlluminationTexture(C).Load(int3(PixelCoords, 0));
	float4 NewFallbackReflection = g_RWFallbackReflectionTexture[PixelCoords];
	// TODO: the normal should be a low frequency one.
	// here the normal is not denoised. Maybe we should use the denoised normal.
	float3 GeometryNormal = normalize(GetNormalTexture(C).Load(int3(PixelCoords, 0)).xyz - 0.5);
	float4 DenoisedDI = NewDirectIllumination_C;
	float4 DenoisedII_Diffuse = NewIndirectIllumination_Diffuse_C;
	float4 DenoisedFallbackReflection = NewFallbackReflection;
	if(all(PreviousNDC.xy >= -1) && all(PreviousNDC.xy <= 1)) {
		float2 PreviousScreenPosition = NDC2ToScreen(PreviousNDC.xy);
		// Make sure the billinear weights are consistent with gathered texels
		float2 HistoryBillinearPos = PreviousScreenPosition - 0.5f;
		float2 HistoryBillinearSubPixel = frac(HistoryBillinearPos);
		float2 HistoryBillinearPixel = floor(HistoryBillinearPos);
		float2 HistoryGatherUV = (HistoryBillinearPixel + 1.f) * C.InvFilmDimensions;
		float4 HistoryZDepths = GetHistoryZDepthTexture(C).GatherRed(g_PointClampSampler, HistoryGatherUV).wzxy;
		float  PrevLinearDepth = ZDepthToLinear(C, PreviousNDC.z);
		float4 HistoryLinearDepths = float4(
			ZDepthToLinear(C, HistoryZDepths.x),
			ZDepthToLinear(C, HistoryZDepths.y),
			ZDepthToLinear(C, HistoryZDepths.z),
			ZDepthToLinear(C, HistoryZDepths.w)
		);
		float4 DepthDistances = abs(HistoryLinearDepths - PrevLinearDepth);
		float  Noise = InterleavedGradientNoise(NDC2ToUV(NDC.xy), UB.FrameIndex);
		float4 DepthThresholds = UB.DI_Denoiser_DepthThreshold * lerp(0.5, 1.5, Noise);
		// UE's resolution for disocclusion misses on geometry edges
		DepthThresholds /= clamp(saturate(dot(ViewDirection, GeometryNormal)), .1f, 1.0f); 
		float4 OcclusionWeights   = select(DepthDistances < PrevLinearDepth * DepthThresholds, 1, 0);
		float4 BillinearWeights = float4(
			(1.f - HistoryBillinearSubPixel.x) * (1.f - HistoryBillinearSubPixel.y),
			HistoryBillinearSubPixel.x * (1.f - HistoryBillinearSubPixel.y),
			(1.f - HistoryBillinearSubPixel.x) * HistoryBillinearSubPixel.y,
			HistoryBillinearSubPixel.x * HistoryBillinearSubPixel.y
		);
		float4 Weights = OcclusionWeights * BillinearWeights;
		Weights = Weights / max(dot(Weights, 1), 1e-6f);

		float3 OldDirectIllumination = 0.f;
		float3 OldIndirectIllumination_Diffuse = 0.f;
		float3 OldFallbackReflection = 0.f;
		Texture2D<float4> HistoryDI = GetHistoryDirectIlluminationTexture(C);
		Texture2D<float4> HistoryII_Diffuse = GetHistoryIndirectIlluminationTexture(C);
		{
			float2 BaseUV = ScreenToUV(HistoryBillinearPixel + 0.5f);
			float3 R00 = HistoryDI.SampleLevel(g_PointClampSampler, BaseUV, 0).rgb;
			float3 R10 = HistoryDI.SampleLevel(g_PointClampSampler, BaseUV + float2(C.FilmTexelSize.x, 0), 0).rgb;
			float3 R01 = HistoryDI.SampleLevel(g_PointClampSampler, BaseUV + float2(0, C.FilmTexelSize.y), 0).rgb;
			float3 R11 = HistoryDI.SampleLevel(g_PointClampSampler, BaseUV + C.FilmTexelSize, 0).rgb;
			OldDirectIllumination = R00 * Weights.x + R10 * Weights.y + R01 * Weights.z + R11 * Weights.w;
		}
		{
			float2 BaseUV = ScreenToUV(HistoryBillinearPixel + 0.5f);
			float3 R00 = HistoryII_Diffuse.SampleLevel(g_PointClampSampler, BaseUV, 0).rgb;
			float3 R10 = HistoryII_Diffuse.SampleLevel(g_PointClampSampler, BaseUV + float2(C.FilmTexelSize.x, 0), 0).rgb;
			float3 R01 = HistoryII_Diffuse.SampleLevel(g_PointClampSampler, BaseUV + float2(0, C.FilmTexelSize.y), 0).rgb;
			float3 R11 = HistoryII_Diffuse.SampleLevel(g_PointClampSampler, BaseUV + C.FilmTexelSize, 0).rgb;
			OldIndirectIllumination_Diffuse = R00 * Weights.x + R10 * Weights.y + R01 * Weights.z + R11 * Weights.w;
		}
			{
			float2 BaseUV = ScreenToUV(HistoryBillinearPixel + 0.5f);
			float3 R00 = g_HistoryFallbackReflectionTexture.SampleLevel(g_PointClampSampler, BaseUV, 0).rgb;
			float3 R10 = g_HistoryFallbackReflectionTexture.SampleLevel(g_PointClampSampler, BaseUV + float2(C.FilmTexelSize.x, 0), 0).rgb;
			float3 R01 = g_HistoryFallbackReflectionTexture.SampleLevel(g_PointClampSampler, BaseUV + float2(0, C.FilmTexelSize.y), 0).rgb;
			float3 R11 = g_HistoryFallbackReflectionTexture.SampleLevel(g_PointClampSampler, BaseUV + C.FilmTexelSize, 0).rgb;
			OldFallbackReflection = R00 * Weights.x + R10 * Weights.y + R01 * Weights.z + R11 * Weights.w;
		}

		float4 HistorySampleCounts_DI = HistoryDI.GatherAlpha(g_PointClampSampler, HistoryGatherUV).wzxy;
		float4 HistorySampleCounts_II_Diffuse = HistoryII_Diffuse.GatherAlpha(g_PointClampSampler, HistoryGatherUV).wzxy;
		float4 HistorySampleCounts_FallbackReflection = g_HistoryFallbackReflectionTexture.GatherAlpha(g_PointClampSampler, HistoryGatherUV).wzxy;
		{
			float NewSampleCount_DI = NewDirectIllumination_C.a;
			float OldSampleCount = dot(HistorySampleCounts_DI * Weights, 1.f.xxxx);
			if(UB.DI_NoTemporalDenoising) {
				OldSampleCount = 0;
			}
			float ClampedOldSampleCount = min(OldSampleCount, max(UB.DI_Denoiser_TargetNumSamples - NewSampleCount_DI, 0));
			float SampleCount = ClampedOldSampleCount + NewSampleCount_DI;
			float BlendingFactor = ClampedOldSampleCount / max(SampleCount, 1e-4f);
			DenoisedDI = float4(
				lerp(NewDirectIllumination_C.rgb, OldDirectIllumination, BlendingFactor),
				SampleCount
			);
		}
		{
			float NewSampleCount_II_Diffuse = NewIndirectIllumination_Diffuse_C.a;
			float OldSampleCount = dot(HistorySampleCounts_II_Diffuse * Weights, 1.f.xxxx);
			if(UB.II_NoTemporalDenoising) {
				OldSampleCount = 0;
			}
			float ClampedOldSampleCount = min(OldSampleCount, max(UB.II_Denoiser_TargetNumSamples - NewSampleCount_II_Diffuse, 0));
			float SampleCount = ClampedOldSampleCount + NewSampleCount_II_Diffuse;
			float BlendingFactor = ClampedOldSampleCount / max(SampleCount, 1e-4f);
			DenoisedII_Diffuse = float4(
				lerp(NewIndirectIllumination_Diffuse_C.rgb, OldIndirectIllumination_Diffuse, BlendingFactor),
				SampleCount
			);
		}
		{
			float NewSampleCount_FallbackReflection = NewFallbackReflection.a;
			float OldSampleCount = dot(HistorySampleCounts_FallbackReflection * Weights, 1.f.xxxx);
			if(UB.FallbackReflection_NoTemporalDenoising) {
				OldSampleCount = 0;
			}
			float ClampedOldSampleCount = min(OldSampleCount, max(UB.FallbackReflection_Denoiser_TargetNumSamples - NewSampleCount_FallbackReflection, 0));
			float SampleCount = ClampedOldSampleCount + NewSampleCount_FallbackReflection;
			float BlendingFactor = ClampedOldSampleCount / max(SampleCount, 1e-4f);
			DenoisedFallbackReflection = float4(
				lerp(NewFallbackReflection.rgb, OldFallbackReflection, BlendingFactor),
				SampleCount
			);
		}
	}
	// Store the results back to the direct illumination buffer
	GetRWFilteredDirectIlluminationTexture(C)[PixelCoords] = DenoisedDI;
	GetRWFilteredIndirectIlluminationTexture(C)[PixelCoords] = DenoisedII_Diffuse;
	g_RWFallbackReflectionTexture[PixelCoords] = DenoisedFallbackReflection;
}