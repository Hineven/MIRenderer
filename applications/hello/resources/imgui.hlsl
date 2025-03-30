struct ImGuiVertInput {
    float2 pos : pos;
    float2 uv  : uv;
    float col  : col;
};

struct ImGuiVertOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : TEXCOORD1;
};

float2 Scale;

ImGuiVertOutput ImGuiVS(ImGuiVertInput input) {
    ImGuiVertOutput output;
    output.pos = float4(2 * (float2(0.f, 1.f) + float2(input.pos.x, -input.pos.y) * Scale) - 1, 0, 1);
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
    output.OutColor = input.col;
    return output;
}