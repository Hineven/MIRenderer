#include "headers/Packing.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/SphericalHarmonics.hlsl"
#include "resources/RenderableResources.hlsl"
struct Gaussian3D {
    float3 Position;
    float3 Scale;
    float4 Rotation; // Quaternion
    float  Alpha;
};
struct PackedGaussian3D {
    float3 Position;
    uint Rotation;
    float3 Scale;
    float Alpha;
};
StructuredBuffer<PackedGaussian3D> Gaussian3DBuffer;
Gaussian3D UnpackGaussian(PackedGaussian3D PackedG) {
    Gaussian3D G;
    G.Position = PackedG.Position;
    G.Scale    = PackedG.Scale;
    G.Rotation = UnpackQuaternion(PackedG.Rotation);
    G.Alpha    = PackedG.Alpha;
    return G;
}
Gaussian3D FetchGaussian(uint GaussianIndex) {
    return UnpackGaussian(Gaussian3DBuffer[GaussianIndex]);
}

StructuredBuffer<float3> GaussianSHBuffer;
SH3Coefficents FetchGaussianSHCoefficients(uint GaussianIndex) {
    SH3Coefficents SH;
    // Degree 3: 16x3 coefficients
    [unroll]
    for(uint i = 0; i < 16; ++i) {
        SH.Coefficients[i] = GaussianSHBuffer[GaussianIndex * 16 + i];
    }
    return SH;
}
StructuredBuffer<uint> InstanceGaussianIndexOffsetBuffer;
StructuredBuffer<uint> InstanceGaussianCountBuffer;

RWStructuredBuffer<uint> RWActiveGaussianColorBuffer;
RWStructuredBuffer<uint> RWActiveGaussianCount;
RWStructuredBuffer<uint> RWActiveGaussianListBuffer;
RWStructuredBuffer<float> RWActiveGaussianLinearDepthSrcBuffer;
RWStructuredBuffer<uint>  RWActiveGaussianIndirectionSrcBuffer;
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
void FilterActiveGaussiansVS (uint ActiveGaussianRenderableListIndex : SV_InstanceID, uint InstanceGaussianRank : SV_VertexID) {

	// OPTIMIZE: also filter out the gaussians that failed the visibility test
	// for RasterizationDepth and history depth buffer.
    uint RenderableIndex = ActiveGaussianRenderableListBuffer[ActiveGaussianRenderableListIndex];
    RenderableHeader RH = RenderableHeaderBuffer[RenderableIndex];
    // Unpack the gaussian radiance field index
    uint RadianceFieldIndex = asuint(RH.Metadata.x);
	uint InstanceBaseGaussianIndex = InstanceGaussianIndexOffsetBuffer[RadianceFieldIndex];
	uint InstanceNumGaussians = InstanceGaussianCountBuffer[RadianceFieldIndex];
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

// TanFoV: 2 * tan(fov / 2)
// @return The first two rows of Jacobian Matrix.
float3x3 EWAJacobian2 (float3 Mean, float2 TanFoV, float4x4 View) {
    float3 P = mul(View, float4(Mean, 1.0)).xyz;
    const float limx = 0.65f * TanFoV.x;
    const float limy = 0.65f * TanFoV.y;
    const float txtz = P.x / P.z;
    const float tytz = P.y / P.z;
    // Clamp to (fov expanded) frustum
    P.x = clamp(txtz, -limx, limx) * P.z;
    P.y = clamp(tytz, -limy, limy) * P.z;

    float3x3 J = float3x3(
        (2 / TanFoV.x) / P.z, 0.0f, - (2 / TanFoV.x) * P.x / (P.z * P.z),
        0.0f, (2 / TanFoV.y) / P.z, - (2 / TanFoV.y) * P.y / (P.z * P.z),
        0.0f, 0.0f, 0.0f);
    return J;
}

struct SymmetricMatrix {
    float3 Diagonal;
    float3 OffDiagonal;
};

float3x3 ExpandSymmetricMatrix (SymmetricMatrix C) {
    float3x3 MC = float3x3(
        C.Diagonal.x, C.OffDiagonal.x, C.OffDiagonal.y,
        C.OffDiagonal.x, C.Diagonal.y, C.OffDiagonal.z,
        C.OffDiagonal.y, C.OffDiagonal.z, C.Diagonal.z
    );
    return MC;
}

float3 ProjectCovarianceMatrixToNDC(float3x3 J, SymmetricMatrix Covariance3D, float4x4 View)
{
    float3x3 W = float3x3(
        View[0][0], View[0][1], View[0][2],
        View[1][0], View[1][1], View[1][2],
        View[2][0], View[2][1], View[2][2]);

    float3x3 Mk = mul(J, W);
    
    float3x3 C = ExpandSymmetricMatrix(Covariance3D);
    float3x3 C_2D = mul(mul(Mk, C), transpose(Mk));

    return float3(C_2D[0][0], C_2D[0][1], C_2D[1][1]);
}

float3x3 GetRotationScaleTransform (float4 Rotation, float3 Scale) {
    // Rotation matrix
    float3x3 R = BuildRotationMatrix(Rotation);
    // Scaling matrix
    float3x3 S = {
        Scale.x, 0, 0,
        0, Scale.y, 0,
        0, 0, Scale.z
    };
    return mul(R, S);
}

SymmetricMatrix ComputeCovarianceMatrix (float3 Scale, float4 Rotation, float3x3 InstanceRotationScale) {
    float3x3 M = mul(InstanceRotationScale, GetRotationScaleTransform(Rotation, Scale));
    // Covariance matrixs
    float3x3 Covariance = mul(M, transpose(M));

    float3 Diagonal = float3(
        Covariance[0][0],
        Covariance[1][1],
        Covariance[2][2]
    );
    float3 OffDiagonal = float3(
        Covariance[0][1],
        Covariance[0][2],
        Covariance[1][2]
    );
    SymmetricMatrix Ret = (SymmetricMatrix)0;
    Ret.Diagonal = Diagonal;
    Ret.OffDiagonal = OffDiagonal;
    return Ret;
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
    float3x3 J = EWAJacobian2(GaussianWorldPosition, C.TanFoVY, C.WorldToView);

    SymmetricMatrix WorldCov3D = ComputeCovarianceMatrix(G.Scale, G.Rotation, To3x3(InstanceTransform));

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

// Dummy pixel shader for filter pass (writes nothing, required for graphics pipeline creation)
float4 FilterActiveGaussiansPS() : SV_Target0 { return float4(0,0,0,0); }

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
    G.Scale = max(G.Scale, 2e-4f);
    float3x3 InvScaleM = float3x3(
        1.0f / G.Scale.x, 0, 0,
        0, 1.0f / G.Scale.y, 0,
        0, 0, 1.0f / G.Scale.z
    );
    float3x3 R = BuildRotationMatrix(G.Rotation);
    InvCov = mul(InvScaleM, transpose(R));
    InvCov = mul(transpose(InvCov), InvCov);
    float3 Temp = mul(InvCov, Direction);
    float  Numerator  = dot(G.Position - Origin, Temp);
    float  Denominator = dot(Direction, Temp);
    return Numerator / max(Denominator, 1e-7f);
}

float Evaluate2DUnnormalizedGaussian (float2 P) {
    // float NormalizationFactor = 1 / (2 * PI);
    return exp(-dot(P, P) / 2);
}

float Evaluate2DGaussian (float2 P) {
    float NormalizationFactor = 1 / (2 * PI);
    return NormalizationFactor * exp(-dot(P, P) / 2);
}

[maxvertexcount(6)]
void DrawActiveGaussians_GS(point DrawActiveGaussians_GSInput Input[1], inout TriangleStream<DrawActiveGaussians_GSOutput> TriStream)
{
    int ActiveListIndex = ActiveGaussianIndirectionBuffer[Input[0].PrimitiveIndex];
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
    // GaussianIndex = max(0, min(GaussianIndex, 10000));
    // GaussianPBR G_PBR = FetchGaussianPBR(GaussianIndex);

    // SimpleMaterial M = g_MaterialBuffer[InstanceIndex];
    // Scale color & roughness
    // {
    //     float3 ColorScaler = M.Albedo;
    //     G_PBR.Albedo = saturate(G_PBR.Albedo * ColorScaler);
    //     G_PBR.Albedo = max(G_PBR.Albedo, M.Emissive);
    //     G_PBR.Roughness = saturate(G_PBR.Roughness * M.Roughness);
    // }
    
    float3 Color = saturate(UnpackUnorm4x8(RWActiveGaussianColorBuffer[ActiveListIndex]).rgb);
    float4 ColorAlpha = float4(Color, G.Alpha);
    
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
    Output.Position = float4(Left1, LinearDepthToZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(-1,  0.5);
    Output.Position = float4(Left2, LinearDepthToZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0, 0.75))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(0, -1);
    Output.Position = float4(Top, LinearDepthToZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 0))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(0, 1);
    Output.Position = float4(Bottom, LinearDepthToZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(0.5, 1))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(1, -0.5);
    Output.Position = float4(Right1, LinearDepthToZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.25))), 1);
    TriStream.Append(Output);

    Output.UVW.xy = Expand * float2(1,  0.5);
    Output.Position = float4(Right2, LinearDepthToZDepth(C, InterpolateBarycentrics(D_TL, D_TR, D_BL, float2(1, 0.75))), 1);
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
