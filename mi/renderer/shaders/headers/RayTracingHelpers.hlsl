#ifndef RAY_TRACING_HELPERS_H
#define RAY_TRACING_HELPERS_H

RayDesc GetRayDesc () {
    RayDesc Ray;
    Ray.Origin = WorldRayOrigin();
    Ray.Direction = WorldRayDirection();
    Ray.TMin = RayTMin();
    Ray.TMax = RayTCurrent();
    return Ray;
}

// Ray Tracing Gems, Chapter 6: "A Fast and Robust Method for Avoiding Self-Intersection"
// by Carsten Wachter and Nikolaus Binder.
// Offsets the position along the normal by manipulating the IEEE 754 bit representation,
// producing an adaptive offset proportional to the floating-point precision at that scale.
float3 OffsetRayOrigin(float3 pos, float3 normal)
{
    const float origin = 1.0f / 16.0f;
    const float fScale = 3.0f / 65536.0f;
    const float iScale = 3.0f * 256.0f;

    // Per-component integer offset to bit representation of fp32 position.
    int3 iOff = int3(normal * iScale);
    float3 iPos = asfloat(asint(pos) + select(pos < 0.0f, -iOff, iOff));

    // Select per-component between small fixed offset or above variable offset
    // depending on distance to origin.
    float3 fOff = normal * fScale;
    return select(abs(pos) < origin, pos + fOff, iPos);
}

#endif // RAY_TRACING_HELPERS_H