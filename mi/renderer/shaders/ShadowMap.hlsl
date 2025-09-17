#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Conventions.hlsl"

StructuredBuffer<RenderableHeader> RenderableHeaders;
StructuredBuffer<float3x4> RenderableTransforms;
StructuredBuffer<float3x3> RenderableNormalTransforms; // InvTranspose of RenderableTransforms
StructuredBuffer<uint2> RenderableIndexAndMaterialIndex;
ConstantBuffer<DirectionalLightForShadowMap> LightView;

struct VS_Output
{
    float4 PositionL : SV_POSITION; // 光源裁剪空间位置
    float3 WorldNormal : NORMAL;    // 世界空间法线
};

VS_Output VS_Main(DefaultStaticMeshVertex Vertex, uint InstanceIndex : SV_InstanceID)
{
    uint RenderableIndex = RenderableIndexAndMaterialIndex[InstanceIndex].x;
    float3x4 ToWorldTransform = RenderableTransforms[RenderableIndex];
    float3x3 ToWorldNormalTransform = RenderableNormalTransforms[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float3 WorldNormal = normalize(mul(ToWorldNormalTransform, Vertex.Normal));
    float4 PositionL = mul(LightView.LightWorldToNDC, float4(WorldPosition, 1));
    
    VS_Output Output = (VS_Output) 0;
    Output.PositionL = PositionL;
    Output.WorldNormal = WorldNormal;
  
    return Output;
}

// 斜率偏移：用 ndotl 估计曲面相对光方向的斜率（近似）
float ComputeSlopeBias(float3 N, float3 L)
{
    // 当法线接近垂直光线时斜率很大，使用一个稳定的近似，避免除零
    float ndotl = saturate(abs(dot(N, L))); // [0,1]
    float tanTheta = sqrt(saturate(1.0 - ndotl * ndotl)) / max(ndotl, 1e-3);
    return LightView.SlopeBias * tanTheta;
}

struct PS_Output
{
    float4 Moments : SV_Target0; // (z, z^2)
};

PS_Output PS_Main(VS_Output input)
{
    PS_Output Output = (PS_Output) 0;
    
    float z = input.PositionL.z;
    
    float slopeBias = ComputeSlopeBias(input.WorldNormal, normalize(LightView.LightDirWS));

    float z_biased = z + LightView.DepthBias + slopeBias;
    
    z_biased = saturate(z_biased);
    
    float m1 = z_biased;
    float m2 = z_biased * z_biased;
    
    // 数值稳定性：避免 m2 < m1^2 导致负方差
    // 加一点极小噪声可减少条纹
    const float kEpsilon = 1e-6;
    m2 = max(m2, m1 * m1 + kEpsilon);
    
    Output.Moments = float4(m1, m2 ,0, 0);
    
    return Output;
}

