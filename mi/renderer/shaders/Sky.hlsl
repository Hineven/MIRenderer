#include "headers/CommonSamplers.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"

struct VS_Output {
    float4 Position : SV_POSITION;
};

VS_Output VS_Main (
    uint VertexIndex : SV_VERTEXID
) {
    VS_Output Out;
    Out.Position = float4(
        VertexIndex == 1 ? 3.0 : -1.0,
        VertexIndex == 2 ? 3.0 : -1.0,
        0,
        1
    );
    return Out;
}

TextureCube<float4> SkyTexture;

float4 PS_Main (
    float4 Position : SV_POSITION
) : SV_TARGET {
    float2 UV = Position.xy / View.Camera.FilmDimensions;
    float2 NDC = UVToNDC2(UV);
    float3 Direction = NDC2ToCameraDirectionUnnormalized(View.Camera, NDC);
    return SkyTexture.SampleLevel(LinearWrapSampler, Direction, 0);
}