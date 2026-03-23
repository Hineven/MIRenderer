#include "shared/SharedGaussianRadianceField.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/SphericalHarmonics.hlsl"
#include "headers/GaussianSplatting.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "resources/RenderableResources.hlsl"
#include "headers/VertexShaderInstanceIndex.hlsl"
#include "resources/GaussianRadianceFieldResources.hlsl"
#include "headers/Random.hlsl"

#define CONSTANT_GAUSSIAN_DEPTH

RWStructuredBuffer<uint> RWActiveGaussianColorBuffer;
RWStructuredBuffer<uint> RWActiveGaussianCount;
RWStructuredBuffer<uint> RWActiveGaussianListBuffer;
RWStructuredBuffer<float> RWActiveGaussianLinearDepthSrcBuffer;
RWStructuredBuffer<uint>  RWActiveGaussianIndirectionSrcBuffer;
RWStructuredBuffer<uint> RWActiveGaussianIndirectionBuffer;
StructuredBuffer<uint> ActiveGaussianIndirectionBuffer;

RWStructuredBuffer<uint> RWActiveGaussianNDCPositionBuffer;
RWStructuredBuffer<uint> RWActiveGaussianQuadNDCVector0Buffer;
RWStructuredBuffer<uint> RWActiveGaussianQuadNDCVector1Buffer;
#ifdef LARGE_GAUSSIAN_HALF_RESOLUTION_PATH
RWStructuredBuffer<uint> RWSmallGaussianCount;
RWStructuredBuffer<uint> RWSmallGaussianListBuffer;
RWStructuredBuffer<uint> RWLargeGaussianCount;
RWStructuredBuffer<uint> RWLargeGaussianListBuffer;
#endif

StructuredBuffer<uint> ActiveGaussianRenderableCount;
StructuredBuffer<uint> ActiveGaussianRenderableListBuffer; // Maintained on host side
StructuredBuffer<uint> DrawGaussianCount;

struct GaussianRadianceFieldUB {
    float GaussianClampingScale;
    float GaussianExpandFactor;
    float StochasticSplitShortAxisThreshold;
    uint Padding;
};
ConstantBuffer<GaussianRadianceFieldUB> UB;

[numthreads(1, 1, 1)]
void ClearCounters () {
    RWActiveGaussianCount[0] = 0;
#ifdef LARGE_GAUSSIAN_HALF_RESOLUTION_PATH
    RWSmallGaussianCount[0] = 0;
    RWLargeGaussianCount[0] = 0;
#endif
}

uint PackActiveGaussianIndex(uint ActiveRenderableListIndex, uint GaussianIndex) {
    return (ActiveRenderableListIndex << 24) | (GaussianIndex & 0x00FFFFFF);
}

void UnpackActiveGaussianIndex(uint PackedIndex, out uint ActiveRenderableListIndex, out uint GaussianIndex) {
    ActiveRenderableListIndex = PackedIndex >> 24;
    GaussianIndex = PackedIndex & 0x00FFFFFF;
}

// Filter the active gaussians that are visible in the view frustrum
void FilterActiveGaussiansVS (
    VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS,
    uint InstanceGaussianRank : SV_VertexID
) {
    uint InstanceID = VS_INSTANCE_INDEX;
    uint ActiveGaussianRenderableListIndex = InstanceID; // + BaseInstance;

	// OPTIMIZE: also filter out the gaussians that failed the visibility test
	// for RasterizationDepth and history depth buffer.
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveGaussianRenderableListIndex];
    RenderableHeader RH = RenderableHeaderBuffer[RenderableIndex];
    // Unpack the gaussian radiance field index
    uint RadianceFieldIndex = asuint(RH.Metadata.x);
    GaussianRadianceFieldHeader FieldHeader = GaussianRadianceFieldHeaderBuffer[RadianceFieldIndex];

	uint InstanceBaseGaussianIndex = FieldHeader.PointOffset;
	uint InstanceNumGaussians      = FieldHeader.NumPoints;
	if(InstanceGaussianRank >= InstanceNumGaussians) return ;
	uint GaussianIndex = InstanceBaseGaussianIndex + InstanceGaussianRank;
	float3x4 Transform = RenderableTransformBuffer[RenderableIndex];

    Gaussian3D G = FetchGaussian(GaussianIndex);
	float3 TransformedPosition = TransformPoint(Transform, G.Position);

    CameraParameters C = GetActiveCamera();
	float3 ViewSpacePosition = 0;
	// We only clip gaussians by their positions in this pass.
	if(IsPointInFrustrum(
         C, TransformedPosition, ViewSpacePosition,
		 false, C.NearPlane, 0.1f
	)) {
		int GaussianCount = WaveActiveCountBits(1);
		int GaussianRank  = WavePrefixCountBits(1);
		int ActiveListBaseIndex = 0;
		if(WaveIsFirstLane()) {
			InterlockedAdd(RWActiveGaussianCount[0], GaussianCount, ActiveListBaseIndex);
		}
		ActiveListBaseIndex = WaveReadLaneFirst(ActiveListBaseIndex);
		// Store the indices of active gaussians into a list.
		int ActiveListIndex = ActiveListBaseIndex + GaussianRank;

		RWActiveGaussianListBuffer[ActiveListIndex] = PackActiveGaussianIndex(ActiveGaussianRenderableListIndex, GaussianIndex);
		RWActiveGaussianIndirectionSrcBuffer[ActiveListIndex] = ActiveListIndex;
		// The view space align the view direction with the negative z axis
		float LinearDepth = - ViewSpacePosition.z;
		RWActiveGaussianLinearDepthSrcBuffer[ActiveListIndex] = LinearDepth;
	}
}

[numthreads(WAVE_SIZE, 1, 1)]
void ProjectActiveGaussians (uint DispatchID : SV_DispatchThreadID) {
	if(DispatchID >= RWActiveGaussianCount[0]) return;
	uint ActiveIndex = DispatchID; // Not sorted for now, so we can use the dispatch id directly
	uint GaussianIndex, ActiveRenderableListIndex;
	UnpackActiveGaussianIndex(RWActiveGaussianListBuffer[ActiveIndex], ActiveRenderableListIndex, GaussianIndex);
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveRenderableListIndex];

    CameraParameters C = GetActiveCamera();
    Gaussian3D G = FetchGaussian(GaussianIndex);
    // Project to 2D covariance

    float3x4 InstanceTransform = RenderableTransformBuffer[RenderableIndex];
    float3 GaussianWorldPosition = TransformPoint(InstanceTransform, G.Position);
    // Employ EWA jacobian if the camera is perspective.
    // Build 2D FoV in tangent space: X depends on aspect ratio
    float TanHalfFovY = C.TanFoVY_2;
    float2 TanFoV = float2(2.0f * C.FilmAspectRatioAndInvAspectRatio.x * TanHalfFovY,
                           2.0f * TanHalfFovY);
    float3x3 J = EWAJacobian2(GaussianWorldPosition, TanFoV, C.WorldToView);

    SymmetricMatrix3D WorldCov3D = ComputeCovarianceMatrix(G.Scales, G.Rotation, To3x3(InstanceTransform));

    // float4x4 LocalToView = mul(C.View, ExpandMatrixWithIdentities(InstanceTransform));
	float4 HomogeneousW = mul(C.WorldToNDC, float4(GaussianWorldPosition, 1));
    float3 Homogeneous = HomogeneousW.xyz / HomogeneousW.w;

    // Get the covariance matrix in NDC space ([-1, 1] ^ 2)
    float3 Cov2D = ProjectCovarianceMatrixToNDC(
        J, WorldCov3D, C.WorldToView
    );

	{
		// 24.12.16: Expand the 2d projection of the gaussian.
		// The data we rendered is trained by a forwarding process with such biasing.
		// So we must expand the gaussian in the same way, otherwise there're
		// empty holes in the output.
		const float H_Var = 0.3f; // The magic number from the original implementation
		// Convert to NDC space for our method
		float VarScaleX = 2.f / C.FilmDimensions.x;
		float VarScaleY = 2.f / C.FilmDimensions.y;
		VarScaleX = VarScaleX * VarScaleX;
		VarScaleY = VarScaleY * VarScaleY;
		float H_VarX = H_Var * VarScaleX;
		float H_VarY = H_Var * VarScaleY;
		Cov2D.x += H_VarX;
		Cov2D.z += H_VarY;
	}

    float  Det           = Cov2D.x * Cov2D.z - Cov2D.y * Cov2D.y;

	SH3Coefficents SH3 = FetchGaussianSHCoefficients(GaussianIndex);
    float3 LocalCameraPosition = TransformPoint(RenderableInverseTransformBuffer[RenderableIndex], C.Position);
    float3 LocalViewDirection = normalize(LocalCameraPosition - G.Position);
	float3 Color = SH3Evaluate(LocalViewDirection, SH3, 2);
    Color = max(Color + 0.5f, 0.f); // Gaussian color biasing. Coherent with training process.
    // Inverse mapping SRGB to linear if input gaussian colors are stored in SRGB space
    RenderableHeader RH = RenderableHeaderBuffer[RenderableIndex];
    // Unpack the gaussian radiance field index
    uint RadianceFieldIndex = asuint(RH.Metadata.x);
    GaussianRadianceFieldHeader FieldHeader = GaussianRadianceFieldHeaderBuffer[RadianceFieldIndex];
    if(FieldHeader.SRGBColorSpace != 0) {
        Color = SRGBColorToLinearColor(Color);
    }
    float Opacity = G.Opacity;
	RWActiveGaussianColorBuffer[ActiveIndex] = PackUnorm4x8(float4(Color, Opacity));

    // Compute extent in screen space (by finding eigenvalues of
    // 2D covariance matrix). Use extent to compute a bounding rectangle
    // of screen-space tiles that this Gaussian overlaps with. Quit if
    // rectangle covers 0 tiles. 
    // Eigenvalues of 2D covariance matrix denotes the variance of the 
    // Gaussian in the directions of the eigenvectors, which are the
    // principal axes of the Gaussian. 
    float Mean    = 0.5f * (Cov2D.x + Cov2D.z);
	float Discriminant = sqrt(max(0.0f, Mean * Mean - Det));
    float Lambda1 = Mean + Discriminant;
    float Lambda2 = Mean - Discriminant;
    // Find the eigenvectors of the covariance matrix
    float2 Eigenvector1 = float2(Cov2D.y, Lambda1 - Cov2D.x);
    float2 Eigenvector2 = float2(Cov2D.y, Lambda2 - Cov2D.x);
    Eigenvector1 = normalize(Eigenvector1);
    Eigenvector2 = normalize(Eigenvector2);
	// Output the projected quad vectors
	// OPTIMIZE: scale the vectors according to opacity of the gaussian
	float2 ClampedPosition = saturateDown(Homogeneous.xy * 0.25 + 0.5f);
	RWActiveGaussianNDCPositionBuffer[ActiveIndex] = PackUnorm2x16(ClampedPosition);
	float LinearDepth = RWActiveGaussianLinearDepthSrcBuffer[ActiveIndex];
	float Bias = 0;
	Lambda1 = abs(Lambda1) + Bias*Bias;
	Lambda2 = abs(Lambda2) + Bias*Bias;
 	// Larger objects have larger clamping scale
	float SqrLambda1 = sqrt(Lambda1);
	float SqrLambda2 = sqrt(Lambda2);
#ifdef LARGE_GAUSSIAN_HALF_RESOLUTION_PATH
        float2 PixelScale = 0.5f * float2(C.FilmDimensions);
        float AxisPixels1 = length(Eigenvector1 * SqrLambda1 * PixelScale);
        float AxisPixels2 = length(Eigenvector2 * SqrLambda2 * PixelScale);
        float ShortAxisPixels = UB.GaussianExpandFactor * min(AxisPixels1, AxisPixels2);
        uint BucketListIndex = 0;
        if (ShortAxisPixels > UB.StochasticSplitShortAxisThreshold) {
            InterlockedAdd(RWLargeGaussianCount[0], 1, BucketListIndex);
            RWLargeGaussianListBuffer[BucketListIndex] = ActiveIndex;
        } else {
            InterlockedAdd(RWSmallGaussianCount[0], 1, BucketListIndex);
            RWSmallGaussianListBuffer[BucketListIndex] = ActiveIndex;
        }
#endif
    float2 ClampedVector1 = Eigenvector1 * SqrLambda1;
	RWActiveGaussianQuadNDCVector0Buffer[ActiveIndex] = PackUnorm2x16(saturateDown(ClampedVector1 * 0.5 + 0.5));
	float2 ClampedVector2 = Eigenvector2 * SqrLambda2;
	RWActiveGaussianQuadNDCVector1Buffer[ActiveIndex] = PackUnorm2x16(saturateDown(ClampedVector2 * 0.5 + 0.5));
}

struct DrawActiveGaussians_GSInput
{
    uint PrimitiveIndex : TEXCOORD0;
};

DrawActiveGaussians_GSInput DrawActiveGaussians_VS (
    uint VertexIndex : SV_VertexID
) {
    DrawActiveGaussians_GSInput Input;
    // In reverse order (farthest to nearest)
    Input.PrimitiveIndex = DrawGaussianCount[0] - VertexIndex - 1;
    return Input;
}

struct DrawActiveGaussians_GSOutput
{
    float4 Position : SV_POSITION;
    float3 UVW  : TEXCOORD0;
    float3 RGB  : TEXCOORD1;
};

// bool CullActiveListGaussian(uint ActiveListIndex, out float2 NDCPosition, out float LinearDepth) {
//     uint Level = g_RWActiveGaussianHiZLevelBuffer[ActiveListIndex];
//     NDCPosition = UnpackUnorm16x2(RWActiveGaussianNDCPositionBuffer[ActiveListIndex]) * 4 - 2;
//     LinearDepth = RWActiveGaussianLinearDepthSrcBuffer[ActiveListIndex];
//     float2 UV = NDCPosition * 0.5 + 0.5;
//     float4 DepthValues = g_HiZTexture.GatherRed(g_PointClampSampler, UV, Level);
//     float4 AlphaValues = g_HiATexture.GatherRed(g_PointClampSampler, UV, Level);
//     bool4  AlphaMask = AlphaValues > 0.95f;
//     bool4  DepthMask = DepthValues < LinearDepth;
//     return all(AlphaMask & DepthMask);
// }

// Compute the ray t to evaluate max respose of a ray-gaussian intersection according to the paper.
// @param Origin The origin of the ray
// @param Direction The direction of the ray
// @param G The gaussian to evaluate
// @return The ray t to evaluate the maximum response of the gaussian along the ray
float EvaluateGaussianResponseRayT (float3 Origin, float3 Direction, Gaussian3D G, inout float3x3 InvCov) {
    // Clamp the scale to avoid numerical issues
    G.Scales = max(G.Scales, 2e-4f);
    float3x3 InvScaleM = float3x3(
        1.0f / G.Scales.x, 0, 0,
        0, 1.0f / G.Scales.y, 0,
        0, 0, 1.0f / G.Scales.z
    );
    float3x3 R = BuildRotationMatrix(G.Rotation);
    InvCov = mul(InvScaleM, transpose(R));
    InvCov = mul(transpose(InvCov), InvCov);
    float3 Temp = mul(InvCov, Direction);
    float  Numerator  = dot(G.Position - Origin, Temp);
    float  Denominator = dot(Direction, Temp);
    return Numerator / max(Denominator, 1e-7f);
}

[maxvertexcount(6)]
void DrawActiveGaussians_GS(point DrawActiveGaussians_GSInput Input[1], inout TriangleStream<DrawActiveGaussians_GSOutput> TriStream)
{
    uint ActiveListIndex = ActiveGaussianIndirectionBuffer[Input[0].PrimitiveIndex];
    // if(ActiveListIndex % 4 != UB.FrameIndex % 4) return ;
    // if(CullActiveListGaussian(ActiveListIndex)) return ;
    
    CameraParameters C = GetActiveCamera();
    
    // Output a quad to bound the Gaussian in screen space
    float2 Center  = UnpackUnorm2x16(RWActiveGaussianNDCPositionBuffer[ActiveListIndex]) * 4 - 2;
    float2 Vec1    = -(UnpackUnorm2x16(RWActiveGaussianQuadNDCVector0Buffer[ActiveListIndex]) * 2 - 1);
    float2 Vec2    = -(UnpackUnorm2x16(RWActiveGaussianQuadNDCVector1Buffer[ActiveListIndex]) * 2 - 1);
    // Expand the quad to be conservative
    float  Expand  = UB.GaussianExpandFactor;
    float2 Top    = Center + Expand * -Vec1;
    float2 Bottom = Center + Expand *  Vec1;
    float2 Vec1H  = Vec1 * 0.5;
    float2 Left1  = Center + Expand * (-Vec2 -Vec1H);
    float2 Left2  = Center + Expand * (-Vec2 +Vec1H);
    float2 Right1 = Center + Expand * ( Vec2 -Vec1H);
    float2 Right2 = Center + Expand * ( Vec2 +Vec1H);
    
    float  Depth   =  RWActiveGaussianLinearDepthSrcBuffer[ActiveListIndex];
    
    // float2 Depth01 = g_RWActiveGaussianQuadLinearDepthSrcBuffer[ActiveListIndex];
    // Magnify according to the expand parameter
    // Depth01 = Depth + (Depth - Depth01) * Expand;
    uint GaussianIndex, ActiveRenderableListIndex;
    UnpackActiveGaussianIndex(RWActiveGaussianListBuffer[ActiveListIndex], ActiveRenderableListIndex, GaussianIndex);
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveRenderableListIndex];
    float3x4 RenderableToWorld = RenderableTransformBuffer[RenderableIndex];
    Gaussian3D G = FetchGaussian(GaussianIndex);
    float3x3 InvCov;
    float3 Direction0 = NDC2ToCameraDirectionUnnormalized(C, Top);
    float3 Direction1 = NDC2ToCameraDirectionUnnormalized(C, 0.5 * (Left1 + Left2));
    float3 RayOrigin0 = NDC2ToCameraOrigin(C, Top);
    float3 RayOrigin1 = NDC2ToCameraOrigin(C, 0.5 * (Left1 + Left2));
    float3 InstanceLocalOrigin0    = TransformPoint(RenderableToWorld, RayOrigin0);
    float3 InstanceLocalOrigin1    = TransformPoint(RenderableToWorld, RayOrigin1);
    float3 InstanceLocalDirection0 = TransformVector(RenderableToWorld, Direction0);
    float3 InstanceLocalDirection1 = TransformVector(RenderableToWorld, Direction1);
    float2 Depth01 = float2(
        EvaluateGaussianResponseRayT(InstanceLocalOrigin0, InstanceLocalDirection0, G, InvCov),
        EvaluateGaussianResponseRayT(InstanceLocalOrigin1, InstanceLocalDirection1, G, InvCov)
    );
#ifdef CONSTANT_GAUSSIAN_DEPTH
    Depth01.xy = Depth.xx;
#endif
    float  D_TL    =  Depth01.x +Depth01.y - Depth;
    float  D_TR    =  Depth01.x -Depth01.y + Depth;
    float  D_BL    =  Depth01.y -Depth01.x + Depth;

    float3 Color = saturate(UnpackUnorm4x8(RWActiveGaussianColorBuffer[ActiveListIndex]).rgb);
    float4 ColorAlpha = float4(Color, G.Opacity);
    
    // Is the quantilization affecting render quality?
    // int Seed = UB.FrameIndex + InstanceIndex * 77183 + GaussianIndex * 81937121;
    // float Q_Noise = 0.2 * (frac(sin(Seed) * 43758.5453) - 0.5f);
    // ColorAlpha.rgb = saturate(ColorAlpha.rgb + Q_Noise);

    float  Alpha = ColorAlpha.w;
    float InvFarPlane = 1 / C.FarPlane;

    DrawActiveGaussians_GSOutput Output;

    Output.UVW.z = Alpha;
    Output.RGB = Color.rgb;

    Output.UVW.xy = Expand * float2(-1, -0.5);
    Output.Position = float4(Left1, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(-1,  0.5);
    Output.Position = float4(Left2, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.75))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(0, -1);
    Output.Position = float4(Top, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 0))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(0, 1);
    Output.Position = float4(Bottom, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 1))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(1, -0.5);
    Output.Position = float4(Right1, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(1,  0.5);
    Output.Position = float4(Right2, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.75))), 1);
    TriStream.Append(Output);
    
    TriStream.RestartStrip();
}

struct DrawActiveGaussians_PSInput
{
    float4 Position : SV_Position;
    float3 UVW : TEXCOORD0;
    float3 RGB : TEXCOORD1;
};

struct GBufferOutput {
    float4 ColorAlpha    : SV_Target0;
};

GBufferOutput DrawActiveGaussians_PS (DrawActiveGaussians_PSInput Input) {
    float2 UV     = Input.UVW.xy;
    float4 RGBA   = float4(Input.RGB.rgb, Input.UVW.z);
    float  Alpha  = RGBA.w *  Evaluate2DUnnormalizedGaussian(UV);
    if (Alpha < 0.01f) discard; // Early cull to save bandwidth. The threshold is a magic number that works well in practice.
    
    float3 Color = saturate(RGBA.xyz);
    CameraParameters C = GetActiveCamera();
    float  LinearDepth  = ZDepthToLinearDepth(C, Input.Position.z);
    GBufferOutput Result = (GBufferOutput)0;
    Result.ColorAlpha    = float4(Color, Alpha);
    return Result;
}


DrawActiveGaussians_GSInput StochasticDrawActiveGaussians_VS (
    uint VertexIndex : SV_VertexID
) {
    DrawActiveGaussians_GSInput Input;
    // In reverse order (farthest to nearest)
    Input.PrimitiveIndex = DrawGaussianCount[0] - VertexIndex - 1;
    return Input;
}

struct StochasticDrawActiveGaussians_GSOutput
{
    float4 Position : SV_POSITION;
    float4 UVWS : TEXCOORD0;
#ifdef STOCHASTIC_GATHERING_PATH
    // Use stochastic gaussian gathering rendering path, encode the active gaussian index into the shader input
    uint   ActiveGaussianIndex : TEXCOORD1;
#else
    float3 RGB  : TEXCOORD1;
#endif
};

struct StochasticDrawActiveGaussians_PSInput
{
    float4 Position : SV_Position;
    float4 UVWS : TEXCOORD0;
#ifdef STOCHASTIC_GATHERING_PATH
    uint   ActiveGaussianIndex : TEXCOORD1;
#else
    float3 RGB : TEXCOORD1;
#endif
};

struct StochasticDrawLargeGaussianIndex_GSOutput
{
    float4 Position : SV_POSITION;
    float3 UVW : TEXCOORD0;
    nointerpolation uint ActiveListIndex : TEXCOORD1;
    nointerpolation uint Seed : TEXCOORD2;
};

struct StochasticDrawLargeGaussianIndex_PSInput
{
    float4 Position : SV_Position;
    float3 UVW : TEXCOORD0;
    nointerpolation uint ActiveListIndex : TEXCOORD1;
    nointerpolation uint Seed : TEXCOORD2;
};

[maxvertexcount(6)]
void StochasticDrawActiveGaussians_GS(point DrawActiveGaussians_GSInput Input[1], inout TriangleStream<StochasticDrawActiveGaussians_GSOutput> TriStream)
{
    uint ActiveListIndex = ActiveGaussianIndirectionBuffer[Input[0].PrimitiveIndex];
    // if(ActiveListIndex % 4 != UB.FrameIndex % 4) return ;
    // if(CullActiveListGaussian(ActiveListIndex)) return ;
    
    CameraParameters C = GetActiveCamera();
    
    // Output a quad to bound the Gaussian in screen space
    float2 Center  = UnpackUnorm2x16(RWActiveGaussianNDCPositionBuffer[ActiveListIndex]) * 4 - 2;
    float2 Vec1    = -(UnpackUnorm2x16(RWActiveGaussianQuadNDCVector0Buffer[ActiveListIndex]) * 2 - 1);
    float2 Vec2    = -(UnpackUnorm2x16(RWActiveGaussianQuadNDCVector1Buffer[ActiveListIndex]) * 2 - 1);
    // Expand the quad to be conservative
    float  Expand  = UB.GaussianExpandFactor;
    float2 Top    = Center + Expand * -Vec1;
    float2 Bottom = Center + Expand *  Vec1;
    float2 Vec1H  = Vec1 * 0.5;
    float2 Left1  = Center + Expand * (-Vec2 -Vec1H);
    float2 Left2  = Center + Expand * (-Vec2 +Vec1H);
    float2 Right1 = Center + Expand * ( Vec2 -Vec1H);
    float2 Right2 = Center + Expand * ( Vec2 +Vec1H);
    
    float  Depth   =  RWActiveGaussianLinearDepthSrcBuffer[ActiveListIndex];
    
    // float2 Depth01 = g_RWActiveGaussianQuadLinearDepthSrcBuffer[ActiveListIndex];
    // Magnify according to the expand parameter
    // Depth01 = Depth + (Depth - Depth01) * Expand;
    uint GaussianIndex, ActiveRenderableListIndex;
    UnpackActiveGaussianIndex(RWActiveGaussianListBuffer[ActiveListIndex], ActiveRenderableListIndex, GaussianIndex);
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveRenderableListIndex];
    float3x4 RenderableToWorld = RenderableTransformBuffer[RenderableIndex];
    Gaussian3D G = FetchGaussian(GaussianIndex);
    float3x3 InvCov;
    float3 Direction0 = NDC2ToCameraDirectionUnnormalized(C, Top);
    float3 Direction1 = NDC2ToCameraDirectionUnnormalized(C, 0.5 * (Left1 + Left2));
    float3 RayOrigin0 = NDC2ToCameraOrigin(C, Top);
    float3 RayOrigin1 = NDC2ToCameraOrigin(C, 0.5 * (Left1 + Left2));
    float3 InstanceLocalOrigin0    = TransformPoint(RenderableToWorld, RayOrigin0);
    float3 InstanceLocalOrigin1    = TransformPoint(RenderableToWorld, RayOrigin1);
    float3 InstanceLocalDirection0 = TransformVector(RenderableToWorld, Direction0);
    float3 InstanceLocalDirection1 = TransformVector(RenderableToWorld, Direction1);
    float2 Depth01 = float2(
        EvaluateGaussianResponseRayT(InstanceLocalOrigin0, InstanceLocalDirection0, G, InvCov),
        EvaluateGaussianResponseRayT(InstanceLocalOrigin1, InstanceLocalDirection1, G, InvCov)
    );
#ifdef CONSTANT_GAUSSIAN_DEPTH
    Depth01.xy = Depth.xx;
#endif
    float  D_TL    =  Depth01.x +Depth01.y - Depth;
    float  D_TR    =  Depth01.x -Depth01.y + Depth;
    float  D_BL    =  Depth01.y -Depth01.x + Depth;

    float3 Color = saturate(UnpackUnorm4x8(RWActiveGaussianColorBuffer[ActiveListIndex]).rgb);
    float4 ColorAlpha = float4(Color, G.Opacity);
    
    // Is the quantilization affecting render quality?
    // int Seed = UB.FrameIndex + InstanceIndex * 77183 + GaussianIndex * 81937121;
    // float Q_Noise = 0.2 * (frac(sin(Seed) * 43758.5453) - 0.5f);
    // ColorAlpha.rgb = saturate(ColorAlpha.rgb + Q_Noise);

    float  Alpha = ColorAlpha.w;
    float InvFarPlane = 1 / C.FarPlane;

    StochasticDrawActiveGaussians_GSOutput Output;

    Output.UVWS.zw = float2(Alpha, float(0x3FFFF & (ActiveListIndex ^ 0x48f4c)));
#ifdef STOCHASTIC_GATHERING_PATH
    Output.ActiveGaussianIndex = ActiveListIndex;
#else
    Output.RGB = Color.rgb;
#endif
    Output.UVWS.xy = Expand * float2(-1, -0.5);
    Output.Position = float4(Left1, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVWS.xy = Expand * float2(-1,  0.5);
    Output.Position = float4(Left2, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.75))), 1);
    TriStream.Append(Output);

    Output.UVWS.xy = Expand * float2(0, -1);
    Output.Position = float4(Top, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 0))), 1);
    TriStream.Append(Output);

    Output.UVWS.xy = Expand * float2(0, 1);
    Output.Position = float4(Bottom, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 1))), 1);
    TriStream.Append(Output);

    Output.UVWS.xy = Expand * float2(1, -0.5);
    Output.Position = float4(Right1, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVWS.xy = Expand * float2(1,  0.5);
    Output.Position = float4(Right2, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.75))), 1);
    TriStream.Append(Output);
    
    TriStream.RestartStrip();
}

struct StochasticDrawActiveGaussians_PSOutput
{
#ifdef STOCHASTIC_GATHERING_PATH
    uint   GaussianVisibility : SV_Target0;
#else
    float4 ColorAlpha    : SV_Target0;
#endif
};

StochasticDrawActiveGaussians_PSOutput StochasticDrawActiveGaussians_PS (StochasticDrawActiveGaussians_PSInput Input) {
    float2 UV     = Input.UVWS.xy;
    uint Seed     = uint(max(0, int(Input.UVWS.w)));
#ifdef STOCHASTIC_GATHERING_PATH
    uint ActiveGaussianIndex = Input.ActiveGaussianIndex;    
#else
    float3 RGB   = Input.RGB.rgb;
#endif
    float  Alpha  = Input.UVWS.z *  Evaluate2DUnnormalizedGaussian(UV);
    if (Alpha < 0.01f) discard; // Early cull to save bandwidth. The threshold is a magic number that works well in practice.
    CameraParameters C = GetActiveCamera();

    int2 PixelCoords = int2(floor(Input.Position.xy) * float2(C.FilmDimensions));
    uint PixelIndex = asuint(PixelCoords.y) * C.FilmDimensions.x + asuint(PixelCoords.x);
    Random rng = MakeRandom(PixelIndex, Seed + View.FrameIndex);
    float Noise = rng.rand();

    // This seems to produce worse results.
    // TODO: use a rotated blue noise.
    // float Noise = InterleavedGradientNoise(Input.Position.xy, Seed + View.FrameIndex);
    
    if(Alpha < Noise) discard;

    StochasticDrawActiveGaussians_PSOutput Result = (StochasticDrawActiveGaussians_PSOutput)0;
#ifdef STOCHASTIC_GATHERING_PATH
    Result.GaussianVisibility = ActiveGaussianIndex;
#else
    float3 Color = saturate(RGB);
    Result.ColorAlpha    = float4(Color, 1);
#endif
    return Result;
}

DrawActiveGaussians_GSInput StochasticDrawLargeGaussianIndex_VS (
    uint VertexIndex : SV_VertexID
) {
    DrawActiveGaussians_GSInput Input;
    Input.PrimitiveIndex = DrawGaussianCount[0] - VertexIndex - 1;
    return Input;
}

[maxvertexcount(6)]
void StochasticDrawLargeGaussianIndex_GS(point DrawActiveGaussians_GSInput Input[1], inout TriangleStream<StochasticDrawLargeGaussianIndex_GSOutput> TriStream)
{
    uint ActiveListIndex = ActiveGaussianIndirectionBuffer[Input[0].PrimitiveIndex];
    CameraParameters C = GetActiveCamera();

    float2 Center  = UnpackUnorm2x16(RWActiveGaussianNDCPositionBuffer[ActiveListIndex]) * 4 - 2;
    float2 Vec1    = -(UnpackUnorm2x16(RWActiveGaussianQuadNDCVector0Buffer[ActiveListIndex]) * 2 - 1);
    float2 Vec2    = -(UnpackUnorm2x16(RWActiveGaussianQuadNDCVector1Buffer[ActiveListIndex]) * 2 - 1);
    float  Expand  = UB.GaussianExpandFactor;
    float2 Top    = Center + Expand * -Vec1;
    float2 Bottom = Center + Expand *  Vec1;
    float2 Vec1H  = Vec1 * 0.5;
    float2 Left1  = Center + Expand * (-Vec2 -Vec1H);
    float2 Left2  = Center + Expand * (-Vec2 +Vec1H);
    float2 Right1 = Center + Expand * ( Vec2 -Vec1H);
    float2 Right2 = Center + Expand * ( Vec2 +Vec1H);

    float  Depth   =  RWActiveGaussianLinearDepthSrcBuffer[ActiveListIndex];

    uint GaussianIndex, ActiveRenderableListIndex;
    UnpackActiveGaussianIndex(RWActiveGaussianListBuffer[ActiveListIndex], ActiveRenderableListIndex, GaussianIndex);
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveRenderableListIndex];
    float3x4 RenderableToWorld = RenderableTransformBuffer[RenderableIndex];
    Gaussian3D G = FetchGaussian(GaussianIndex);
    float3x3 InvCov;
    float3 Direction0 = NDC2ToCameraDirectionUnnormalized(C, Top);
    float3 Direction1 = NDC2ToCameraDirectionUnnormalized(C, 0.5 * (Left1 + Left2));
    float3 RayOrigin0 = NDC2ToCameraOrigin(C, Top);
    float3 RayOrigin1 = NDC2ToCameraOrigin(C, 0.5 * (Left1 + Left2));
    float3 InstanceLocalOrigin0    = TransformPoint(RenderableToWorld, RayOrigin0);
    float3 InstanceLocalOrigin1    = TransformPoint(RenderableToWorld, RayOrigin1);
    float3 InstanceLocalDirection0 = TransformVector(RenderableToWorld, Direction0);
    float3 InstanceLocalDirection1 = TransformVector(RenderableToWorld, Direction1);
    float2 Depth01 = float2(
        EvaluateGaussianResponseRayT(InstanceLocalOrigin0, InstanceLocalDirection0, G, InvCov),
        EvaluateGaussianResponseRayT(InstanceLocalOrigin1, InstanceLocalDirection1, G, InvCov)
    );
#ifdef CONSTANT_GAUSSIAN_DEPTH
    Depth01.xy = Depth.xx;
#endif
    float  D_TL    =  Depth01.x +Depth01.y - Depth;
    float  D_TR    =  Depth01.x -Depth01.y + Depth;
    float  D_BL    =  Depth01.y -Depth01.x + Depth;

    StochasticDrawLargeGaussianIndex_GSOutput Output;
    Output.UVW.z = G.Opacity;
    Output.ActiveListIndex = ActiveListIndex;
    Output.Seed = 0x3FFFF & (ActiveListIndex ^ 0x48f4c);

    Output.UVW.xy = Expand * float2(-1, -0.5);
    Output.Position = float4(Left1, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(-1,  0.5);
    Output.Position = float4(Left2, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.75))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(0, -1);
    Output.Position = float4(Top, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 0))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(0, 1);
    Output.Position = float4(Bottom, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 1))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(1, -0.5);
    Output.Position = float4(Right1, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(1,  0.5);
    Output.Position = float4(Right2, LinearDepthToReversedZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.75))), 1);
    TriStream.Append(Output);

    TriStream.RestartStrip();
}

struct StochasticDrawLargeGaussianIndex_PSOutput
{
    uint ActiveListIndex : SV_Target0;
    //float Opacity : SV_Target1;
};

StochasticDrawLargeGaussianIndex_PSOutput StochasticDrawLargeGaussianIndex_PS (StochasticDrawLargeGaussianIndex_PSInput Input) {
    float2 UV = Input.UVW.xy;
    float Alpha = Input.UVW.z * Evaluate2DUnnormalizedGaussian(UV);
    if (Alpha < 0.01f) discard;

    CameraParameters C = GetActiveCamera();
    int2 PixelCoords = int2(floor(Input.Position.xy) * float2(C.FilmDimensions));
    uint PixelIndex = asuint(PixelCoords.y) * C.FilmDimensions.x + asuint(PixelCoords.x);
    Random rng = MakeRandom(PixelIndex, Input.Seed + View.FrameIndex);
    float Noise = rng.rand();
    if (Alpha < Noise) discard;

    StochasticDrawLargeGaussianIndex_PSOutput Output;
    Output.ActiveListIndex = Input.ActiveListIndex;
    return Output;
}

Texture2D<uint> LargeGaussianIndex;
Texture2D<float> LargeGaussianDepth;
Texture2D<float> FullResolutionDepth;
RWTexture2D<float4> RWOverlay;
RWTexture2D<float>  RWOpacity;

float3 EvaluateLargeGaussianColor(uint ActiveListIndex, uint2 PixelCoords) {
    CameraParameters C = GetActiveCamera();
    uint GaussianIndex, ActiveRenderableListIndex;
    UnpackActiveGaussianIndex(RWActiveGaussianListBuffer[ActiveListIndex], ActiveRenderableListIndex, GaussianIndex);
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveRenderableListIndex];
    Gaussian3D G = FetchGaussian(GaussianIndex);
    SH3Coefficents SH3 = FetchGaussianSHCoefficients(GaussianIndex);

    float2 NDC2 = ScreenCoordsToNDC2(C, PixelCoords);
    float3 WorldViewDirection = normalize(NDC2ToCameraDirectionUnnormalized(C, NDC2));
    float3 LocalViewDirection = normalize(TransformVector(RenderableInverseTransformBuffer[RenderableIndex], WorldViewDirection));
    float3 Color = SH3Evaluate(LocalViewDirection, SH3, 2);
    Color = max(Color + 0.5f, 0.f);

    RenderableHeader RH = RenderableHeaderBuffer[RenderableIndex];
    uint RadianceFieldIndex = asuint(RH.Metadata.x);
    GaussianRadianceFieldHeader FieldHeader = GaussianRadianceFieldHeaderBuffer[RadianceFieldIndex];
    if(FieldHeader.SRGBColorSpace != 0) {
        Color = SRGBColorToLinearColor(Color);
    }
    return saturate(Color);
}

#ifndef THREAD_GROUP_SIZE
// Just to make other shaders compile, the actual value is defined in the shader that uses this function.
#define THREAD_GROUP_SIZE 8
#endif

[numthreads(THREAD_GROUP_SIZE, THREAD_GROUP_SIZE, 1)]
void ComposeLargeGaussians(uint2 DispatchID : SV_DispatchThreadID) {
    CameraParameters C = GetActiveCamera();
    if (any(DispatchID >= C.FilmDimensions)) return;

    uint LargeWidth, LargeHeight;
    LargeGaussianIndex.GetDimensions(LargeWidth, LargeHeight);
    uint2 LargePixel = min(DispatchID >> 1, uint2(LargeWidth - 1, LargeHeight - 1));

    uint LargeActiveGaussianIndex = LargeGaussianIndex.Load(int3(LargePixel, 0));
    if (IsInvalid(LargeActiveGaussianIndex)) return;

    float LargeDepthValue = LargeGaussianDepth.Load(int3(LargePixel, 0));
    float FullResolutionDepthValue = FullResolutionDepth.Load(int3(DispatchID, 0));
    if (LargeDepthValue >= FullResolutionDepthValue) {
        float3 Color = EvaluateLargeGaussianColor(LargeActiveGaussianIndex, DispatchID);
        RWOverlay[DispatchID] = float4(Color, 1);
    }
}


Texture2D<uint> GaussianVisibilityTexture;

StructuredBuffer<uint> ActiveGaussianColorBuffer;
StructuredBuffer<float> ActiveGaussianLinearDepthSrcBuffer;
StructuredBuffer<uint> ActiveGaussianNDCPositionBuffer;
StructuredBuffer<uint> ActiveGaussianQuadNDCVector0Buffer;
StructuredBuffer<uint> ActiveGaussianQuadNDCVector1Buffer;

#ifndef TILE_SIZE
#define TILE_SIZE 8
#endif

#define RADIUS 1
#define SOURCE_TILE_WIDTH (TILE_SIZE + 2 * RADIUS)   // 10
#define SOURCE_TILE_HEIGHT (TILE_SIZE + 2 * RADIUS)   // 10
#define SOURCE_TEXEL_COUNT (SOURCE_TILE_WIDTH * SOURCE_TILE_HEIGHT)          // 100
#define GATHER_NEIGHBOR_COUNT ((RADIUS * 2 + 1) * (RADIUS * 2 + 1))   // 9

groupshared uint SharedTileActiveListIndex[SOURCE_TEXEL_COUNT];
groupshared uint SharedTileActiveGaussianColor[SOURCE_TEXEL_COUNT];
groupshared uint SharedTileActiveGaussianLinearDepth[SOURCE_TEXEL_COUNT];
groupshared uint SharedTileActiveGaussianVec1[SOURCE_TEXEL_COUNT];
groupshared uint SharedTileActiveGaussianVec2[SOURCE_TEXEL_COUNT];
groupshared uint SharedTileActiveGaussianNDCCenter[SOURCE_TEXEL_COUNT];

uint Hash32(uint x)
{
    // 简单 avalanche hash（非加密）
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint MapElementToBloomMask(uint Value)
{
    uint h1 = Hash32(Value);
    uint b0 = 1u << (h1 & 31u);
    uint b1 = 1u << ((h1>>6) & 31u);
    uint b2 = 1u << ((h1>>12) & 31u);

    return b0 | b1 | b2;
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void StochasticGatheringShading(uint2 DispatchID : SV_DispatchThreadID,
        uint2 GroupID : SV_GroupID,
        uint2 LocalIndex : SV_GroupThreadID)
{
    uint LocalIndex1 = LocalIndex.y * TILE_SIZE + LocalIndex.x;
    const uint GroupSize = TILE_SIZE * TILE_SIZE;

    uint2 BaseCoords = GroupID * TILE_SIZE;

    CameraParameters C = GetActiveCamera();
    

    [unroll]
    for (uint i = LocalIndex1; i < SOURCE_TEXEL_COUNT; i += GroupSize)
    {
        uint OffsetX = i % SOURCE_TILE_WIDTH;
        uint OffsetY = i / SOURCE_TILE_WIDTH;

        int2 TexelCoords = int2(BaseCoords) + int2(OffsetX, OffsetY) - int2(RADIUS, RADIUS);

        TexelCoords.x = clamp(TexelCoords.x, 0, int(C.FilmDimensions.x) - 1);
        TexelCoords.y = clamp(TexelCoords.y, 0, int(C.FilmDimensions.y) - 1);

        uint Index = GaussianVisibilityTexture.Load(int3(TexelCoords, 0));
        SharedTileActiveListIndex[i] = Index;
        if(IsValid(Index)) {
            SharedTileActiveGaussianColor[i] = ActiveGaussianColorBuffer[Index];
            SharedTileActiveGaussianLinearDepth[i] = ActiveGaussianLinearDepthSrcBuffer[Index];
            SharedTileActiveGaussianVec1[i] = ActiveGaussianQuadNDCVector0Buffer[Index];
            SharedTileActiveGaussianVec2[i] = ActiveGaussianQuadNDCVector1Buffer[Index];
            SharedTileActiveGaussianNDCCenter[i] = ActiveGaussianNDCPositionBuffer[Index];
        }
    }

    GroupMemoryBarrierWithGroupSync();

    // Collect a list of identical gaussians for each texel.
    uint ActiveGausisanLocalIndices[GATHER_NEIGHBOR_COUNT];
    uint ActiveGaussianCount = 0, FilledTexelCount = 0;
    if(all(DispatchID < C.FilmDimensions)) {
        uint BloomFilter = 0;

        int2 LocalTexelCoords = RADIUS.xx + LocalIndex;
        for(int dy = -RADIUS; dy <= RADIUS; dy++) {
            for(int dx = -RADIUS; dx <= RADIUS; dx++) {
                int2 NeighborLocalCoords = LocalTexelCoords + int2(dx, dy);
                uint NeighborLocalIndex = NeighborLocalCoords.y * SOURCE_TILE_WIDTH + NeighborLocalCoords.x;
                uint NeighborActiveListIndex = SharedTileActiveListIndex[NeighborLocalIndex];
                uint BloomMask = MapElementToBloomMask(NeighborActiveListIndex);
                if (IsValid(NeighborActiveListIndex)) {
                    FilledTexelCount ++;
                    if ((BloomFilter & BloomMask) != BloomMask) {
                        BloomFilter |= BloomMask;
                        ActiveGausisanLocalIndices[ActiveGaussianCount++] = NeighborLocalIndex;
                    }
                }
            }
        }
        
        // Extract the maximum number of gaussians that can contribute to each pixel.
        int WaveMaxNumActiveGaussians = (int)WaveActiveMax(ActiveGaussianCount);
        // Bubble sorting based on gaussian depths.
        float ActiveGaussianDepths[GATHER_NEIGHBOR_COUNT];
        for(int i = 0; i < WaveMaxNumActiveGaussians; i++) {
            if(i < ActiveGaussianCount) {
                ActiveGaussianDepths[i] = SharedTileActiveGaussianLinearDepth[ActiveGausisanLocalIndices[i]];
            } else ActiveGaussianDepths[i] = 1e9f; // Put invalid gaussians at the end
        }
        for(int i = 0; i < WaveMaxNumActiveGaussians - 1; i++) {
            for(int j = 0; j < WaveMaxNumActiveGaussians - i - 1; j++) {
                if(ActiveGaussianDepths[j] > ActiveGaussianDepths[j + 1]) {
                    // Swap depths
                    float TempDepth = ActiveGaussianDepths[j];
                    ActiveGaussianDepths[j] = ActiveGaussianDepths[j + 1];
                    ActiveGaussianDepths[j + 1] = TempDepth;
                    // Swap indices
                    uint TempIndex = ActiveGausisanLocalIndices[j];
                    ActiveGausisanLocalIndices[j] = ActiveGausisanLocalIndices[j + 1];
                    ActiveGausisanLocalIndices[j + 1] = TempIndex;
                }
            }
        }

        
        // Finally, render the colors
        float Transmittance = 1.f;
        float3 Color = 0.f;
        float2 NDC = ScreenCoordsToNDC2(View.Camera, DispatchID);
        for(int i = 0; i < WaveMaxNumActiveGaussians; i++) {
            if(i < ActiveGaussianCount) {
                uint LocalGaussianIndex = ActiveGausisanLocalIndices[i];
                float4 GaussianColor = UnpackUnorm4x8(SharedTileActiveGaussianColor[LocalGaussianIndex]);

                float2 GaussianCenter  = UnpackUnorm2x16(SharedTileActiveGaussianNDCCenter[LocalGaussianIndex]) * 4 - 2;
                float2 GaussianVec1    = -(UnpackUnorm2x16(SharedTileActiveGaussianVec1[LocalGaussianIndex]) * 2 - 1);
                float2 GaussianVec2    = -(UnpackUnorm2x16(SharedTileActiveGaussianVec2[LocalGaussianIndex]) * 2 - 1);
                float2 GaussianLocal   = NDC - GaussianCenter;
                float2 GaussianScales2 = float2(dot(GaussianVec1, GaussianVec1), dot(GaussianVec2, GaussianVec2));
                float2 FragmentUV      = float2(dot(GaussianLocal, GaussianVec1), dot(GaussianLocal, GaussianVec2)) / GaussianScales2;

                GaussianColor.a *= Evaluate2DUnnormalizedGaussian(FragmentUV);
                Color += Transmittance * GaussianColor.rgb * GaussianColor.a;
                Transmittance *= (1 - GaussianColor.a);
            }
        }
        // Current opacity Oa: 1 - Transmittance
        // Unbiased estimated opacity Ob: NumNonEmpty / (1 + RADIUS*2) ^ 2
        // Color approximation: Ob / max(Oa, 1e-2f) * Color

        float Oa = 1 - Transmittance;
        float Ob = float(FilledTexelCount) / float(GATHER_NEIGHBOR_COUNT);
        float ColorModifier = Ob / max(Oa, 1e-2f);

        // Write out
        RWOverlay[DispatchID] = float4(Color * ColorModifier, Ob);
    }
}