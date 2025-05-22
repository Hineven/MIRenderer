
float4 VS_Main(in uint idx : SV_VertexID) : SV_Position
{
    return 1.0f - float4(4.0f * (idx & 1), 4.0f * (idx >> 1), 1.0f, 0.0f);
}


#define PI 3.14159265359f

struct MappingShaderUB {
    float4x4 ViewProjectionInverse;
    uint2 TextureDimensions;
    uint2 Padding;
};

ConstantBuffer<MappingShaderUB> UB;

Texture2D InEnvironmentMap;

SamplerState InSampler;

float2 SampleSphericalMap(in float3 rd)
{
    return float2(atan2(rd.z, rd.x) / (2.0f * PI) + 0.5f, 1.0f - acos(rd.y) / PI);
}

float4 PS_Main(in float4 pos : SV_Position) : SV_Target
{
    float2 uv  = pos.xy / UB.TextureDimensions;
    float2 ndc = 2.0f * uv - 1.0f;

    float4 world = mul(UB.ViewProjectionInverse, float4(ndc, 1.0f, 1.0f));
    world /= world.w;   // perspective divide

    uv = SampleSphericalMap(normalize(world.xyz));

    float3 color = InEnvironmentMap.Sample(InSampler, uv).xyz;

    return float4(color, 1.0f);
}
