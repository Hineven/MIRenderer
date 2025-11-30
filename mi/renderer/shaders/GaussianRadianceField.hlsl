#include "shared/SharedGaussianRadianceField.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/SphericalHarmonics.hlsl"
#include "headers/GaussianSplatting.hlsl"
#include "headers/Radiometry.hlsl"
#include "resources/RenderableResources.hlsl"
#include "headers/VertexShaderInstanceIndex.hlsl"
#include "resources/GaussianRadianceFieldResources.hlsl"

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

StructuredBuffer<uint> ActiveGaussianRenderableCount;
StructuredBuffer<uint> ActiveGaussianRenderableListBuffer; // Maintained on host side

struct GaussianRadianceFieldUB {
    float GaussianClampingScale;
    float GaussianExpandFactor;
    uint2 Padding;
};
ConstantBuffer<GaussianRadianceFieldUB> UB;

[numthreads(1, 1, 1)]
void ClearCounters () {
    RWActiveGaussianCount[0] = 0;
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
	float3 ViewDirection = normalize(C.Position - GaussianWorldPosition);
	float3 Color = SH3Evaluate(ViewDirection, SH3, 2);
    Color = max(Color + 0.5f, 0.f); // Gaussian color biasing. Coherent with training process.
    // Inverse mapping SRGB to linear if input gaussian colors are stored in SRGB space
    RenderableHeader RH = RenderableHeaderBuffer[RenderableIndex];
    // Unpack the gaussian radiance field index
    uint RadianceFieldIndex = asuint(RH.Metadata.x);
    GaussianRadianceFieldHeader FieldHeader = GaussianRadianceFieldHeaderBuffer[RadianceFieldIndex];
    if(FieldHeader.SRGBColorSpace != 0) {
        Color = SRGBColorToLinearColor(Color);
    }
	RWActiveGaussianColorBuffer[ActiveIndex] = PackUnorm4x8(float4(Color, 0.f));

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
	// Clamp the radius for gaussians (to fix holes for the dataset)
	// TODO: this is totally an empirical trick. But I found it useful.
	// Otherwise my renderer can be visually inconsistent with the original one intensively in
	// some circumstances...
	float Bias = UB.GaussianClampingScale / LinearDepth;
	Lambda1 = abs(Lambda1) + Bias*Bias;
	Lambda2 = abs(Lambda2) + Bias*Bias;
 	// Larger objects have larger clamping scale
	float SqrLambda1 = sqrt(Lambda1);
	float SqrLambda2 = sqrt(Lambda2);
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
    Input.PrimitiveIndex = RWActiveGaussianCount[0] - VertexIndex - 1;
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
    
    float3 Color = saturate(RGBA.xyz);
    CameraParameters C = GetActiveCamera();
    float  LinearDepth  = ZDepthToLinearDepth(C, Input.Position.z);
    GBufferOutput Result = (GBufferOutput)0;
    Result.ColorAlpha    = float4(Color, Alpha);
    return Result;
}
