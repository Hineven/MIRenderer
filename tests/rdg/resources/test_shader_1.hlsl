float4 TestFloat4;
float2 TestFloat2;

RWTexture2D<float4> TestTexture;
RWStructuredBuffer<float4> TestBuffer;

[numthreads(1, 1, 1)]
void TestComputeShaderMain () {
    TestTexture[uint2(0, 0)] = TestFloat4;
    TestTexture[uint2(0, 1)] = float4(TestFloat2, 1, 1);
    TestBuffer[0] = float4(123, 0, 111, 0);
    TestBuffer[1] = TestFloat4;
}

struct VS_Output {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_Output TestGraphicsShaderVS(float3 pos : pos, float2 uv : uv) {
    VS_Output output;
    output.pos = float4(pos, 1);
    output.uv = uv;
    return output;
}

struct PS_Output {
    float4 OutColor : SV_TARGET0;
};

PS_Output TestGraphicsShaderPS(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) {
    PS_Output output = (PS_Output)0;
    output.OutColor = float4(uv, TestFloat2);
    return output;
}
