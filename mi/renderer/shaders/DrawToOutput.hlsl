#include "headers/CommonSamplers.hlsl"

struct DrawToOutputUB {
    float2 InTextureDimensions;
    float  Exposure;
    float  Padding;
};
ConstantBuffer<DrawToOutputUB> UB;

Texture2D<float4> InTexture;

float3 ACEStonemap(float3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float4 VS_Main (uint VertexIndex : SV_VERTEXID) : SV_POSITION {
    return float4(
        VertexIndex == 1 ? 3.0 : -1.0,
        VertexIndex == 2 ? 3.0 : -1.0,
        0,
        1
    );
}

float4 PS_Main (float4 Position : SV_POSITION) : SV_TARGET {
    float2 UV = Position.xy / UB.InTextureDimensions;
    float4 InValue = InTexture.SampleLevel(LinearWrapSampler, UV, 0);
    return float4(ACEStonemap(InValue.rgb * exp(UB.Exposure)), InValue.a);
}

