struct VS_Input {
    float3 pos : pos;
    float2 uv : uv;
};

struct VS_Output {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct SpinningTriangleUB {
    float SpinRadians;
    float3 Padding;
};

ConstantBuffer<SpinningTriangleUB> UB;

VS_Output TriangleVS(VS_Input input) {
    VS_Output output;
    // Spin the triangle around the origin
    float s = sin(UB.SpinRadians);
    float c = cos(UB.SpinRadians);
    output.pos.x = input.pos.x * c - input.pos.y * s;
    output.pos.y = input.pos.x * s + input.pos.y * c;
    output.pos.zw = float2(input.pos.z, 1);
    output.uv = input.uv;
    return output;
}

struct PS_Output {
    float4 OutColor : SV_TARGET0;
};

PS_Output TrianglePS(VS_Output input) {
    PS_Output output = (PS_Output)0;
    output.OutColor = float4(input.uv, 0, 1);
    return output;
}