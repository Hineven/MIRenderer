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
#endif // RAY_TRACING_HELPERS_H