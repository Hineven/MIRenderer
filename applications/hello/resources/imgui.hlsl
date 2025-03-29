struct ImGuiVertInput {
    float2 pos : pos;
    float2 uv  : uv;
    float col  : col;
};

struct ImGuiVertOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float3 col : TEXCOORD1;
};

ImGuiVertOutput ImGuiVS(ImGuiVertInput input) {
    ImGuiVertOutput output;
    output.pos = float4(input.pos, 0, 1);
    output.uv  = input.uv;
    uint input_col = asuint(input.col);
    // Unpack to RGBA
    output.col = float4(
        float((input_col >> 0) & 0xFF),
        float((input_col >> 8) & 0xFF),
        float((input_col >> 16) & 0xFF),
        float((input_col >> 24) & 0xFF)
    ) / 255.0f;
    return output;
}


struct ImGuiFragOutput {
    float4 OutColor : SV_TARGET0;
};
ImGuiFragOutput ImGuiPS(ImGuiVertOutput input) {
    ImGuiFragOutput output;
    output.OutColor = float4(input.col, 1);
    return output;
}