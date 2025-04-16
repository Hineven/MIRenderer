
Texture2D<float4> InTexture;

float4 VS_DrawToOutput (uint VertexIndex : SV_VERTEXID) : SV_POSITION {
    return float4(
        VertexIndex == 1 ? 3.0 : -1.0,
        VertexIndex == 2 ? 3.0 : -1.0,
        0,
        1
    );
}

float4 PS_DrawToOutput (float4 Position : SV_POSITION) : SV_TARGET {
    return InTexture.;
}

