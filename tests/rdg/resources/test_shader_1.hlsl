float4 TestFloat4;
float2 TestFloat2;

RWTexture2D<float4> TestTexture;

[numthreads(1, 1, 1)]
void TestComputeShaderMain () {
    TestTexture[uint2(0, 0)] = TestFloat4;
    TestTexture[uint2(0, 1)] = float4(TestFloat2, 0, 0);
}

struct VS_Output {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_Output VSMain(float4 pos : POSITION, float2 uv : TEXCOORD0) {
    VS_Output output;
    output.pos = pos;
    output.uv = uv;
    return output;
}

