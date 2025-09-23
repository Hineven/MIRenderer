#ifndef CONVENTIONS_HLSL
#define CONVENTIONS_HLSL 

#define INVALID_UINT (0xffffffffu)

bool IsValid (uint v) {
    return v != INVALID_UINT;
}

// Transform a 3x4 matrix to a 4x4 matrix by adding a row of zeros and a one in the last column.
float4x4 float3x4To4x4 (float3x4 m) {
    return float4x4(
        m[0][0], m[0][1], m[0][2], 0.0f,
        m[1][0], m[1][1], m[1][2], 0.0f,
        m[2][0], m[2][1], m[2][2], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    );
}

float3x3 To3x3 (float4x4 m) {
    return float3x3(
        m[0][0], m[0][1], m[0][2],
        m[1][0], m[1][1], m[1][2],
        m[2][0], m[2][1], m[2][2]
    );
}

float3x3 To3x3 (float3x4 m) {
    return float3x3(
        m[0][0], m[0][1], m[0][2],
        m[1][0], m[1][1], m[1][2],
        m[2][0], m[2][1], m[2][2]
    );
}

float select (float a, float b, bool c) {
    return c ? b : a;
}

float3 select (float3 a, float3 b, bool3 c) {
    return float3(
        select(a.x, b.x, c.x),
        select(a.y, b.y, c.y),
        select(a.z, b.z, c.z)
    );
}

float4 select (float4 a, float4 b, bool4 c) {
    return float4(
        select(a.x, b.x, c.x),
        select(a.y, b.y, c.y),
        select(a.z, b.z, c.z),
        select(a.w, b.w, c.w)
    );
}

#endif