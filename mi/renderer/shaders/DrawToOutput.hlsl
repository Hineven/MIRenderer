#include "headers/CommonSamplers.hlsl"

struct DrawToOutputUB {
    float2 InTextureDimensions;
    float2 Padding;
};
ConstantBuffer<DrawToOutputUB> UB;

Texture2D<float4> InTexture;

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
    return InTexture.SampleLevel(LinearWrapSampler, UV, 0);
}

