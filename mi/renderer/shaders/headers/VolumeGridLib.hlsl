#ifndef VOLUME_GRID_LIB_HLSL
#define VOLUME_GRID_LIB_HLSL

#include "../shared/SharedVolumeGrid.hlsl"

// AABB 相交测试
bool IntersectAABB(float3 rayOrigin, float3 rayDir, float3 boxMin, float3 boxMax, out float tNear, out float tFar) {
    float3 invDir = 1.0f / rayDir;
    float3 tbot = invDir * (boxMin - rayOrigin);
    float3 ttop = invDir * (boxMax - rayOrigin);
    float3 tmin = min(ttop, tbot);
    float3 tmax = max(ttop, tbot);
    float2 t = max(tmin.xx, tmin.yz);
    float t0 = max(t.x, t.y);
    t = min(tmax.xx, tmax.yz);
    float t1 = min(t.x, t.y);
    tNear = t0;
    tFar = t1;
    return t1 > max(t0, 0.0f);
}

// 使用 DDA 算法遍历光线穿过的体素，计算路径上的最大密度
float CalculateMaxDensityDDA(Texture3D<float4> tex, float3 localRayOrigin, float3 localRayDir, float tEntry, float tExit, float densityScale) {

    // 1. 获取纹理尺寸
    uint3 dim;
    tex.GetDimensions(dim.x, dim.y, dim.z);
    float3 fDim = float3(dim);

    // 2. 将光线转换到 "体素空间" (0 -> Dim)
    // 注意：光线起点需要偏移到进入点
    float3 startPos = (localRayOrigin + localRayDir * tEntry) * fDim;
    float3 endPos   = (localRayOrigin + localRayDir * tExit)  * fDim;
    float3 rayDirVox = endPos - startPos; // 体素空间的方向向量
    float lenVox = length(rayDirVox);
    if (lenVox < 0.001f) return 0.0f;
    float3 dirStep = rayDirVox / lenVox; // 归一化的体素空间方向

    // DDA 初始化
    int3 voxelPos = clamp(int3(floor(startPos)), int3(0,0,0), int3(dim) - 1);
    int3 step = sign(dirStep);
    float3 deltaT = abs(1.0f / dirStep);
    float3 distToNext;

    // 计算到下一个体素边界的初始距离
    // 如果 dir > 0, 下个边界是 floor(pos) + 1
    // 如果 dir < 0, 下个边界是 floor(pos)
    distToNext.x = (step.x > 0) ? (floor(startPos.x) + 1.0f - startPos.x) * deltaT.x : (startPos.x - floor(startPos.x)) * deltaT.x;
    distToNext.y = (step.y > 0) ? (floor(startPos.y) + 1.0f - startPos.y) * deltaT.y : (startPos.y - floor(startPos.y)) * deltaT.y;
    distToNext.z = (step.z > 0) ? (floor(startPos.z) + 1.0f - startPos.z) * deltaT.z : (startPos.z - floor(startPos.z)) * deltaT.z;

    float maxDensity = 0.0f;
    float tCurrent = 0.0f;
    float tEnd = lenVox; // 在体素空间的总长度

    // DDA 循环 (限制最大步数防止死循环，比如 256 或 512，取决于 Grid 大小和性能权衡)
    // 这里的步数限制是一个 trade-off。
    for (int i = 0; i < 1024; ++i) {
        // 采样当前体素 (使用 Load 避免插值开销，获取原始数据)
        // 注意：这里假设密度在 alpha 通道
        float d = tex.Load(int4(voxelPos, 0)).a;
        maxDensity = max(maxDensity, d);

        // 选择最近的轴推进一步
        if (distToNext.x < distToNext.y && distToNext.x < distToNext.z) {
            tCurrent = distToNext.x;
            distToNext.x += deltaT.x;
            voxelPos.x += step.x;
        } else if (distToNext.y < distToNext.z) {
            tCurrent = distToNext.y;
            distToNext.y += deltaT.y;
            voxelPos.y += step.y;
        } else {
            tCurrent = distToNext.z;
            distToNext.z += deltaT.z;
            voxelPos.z += step.z;
        }

        // 边界检查
        if (tCurrent >= tEnd || any(voxelPos < 0) || any(voxelPos >= int3(dim))) {
            break;
        }
    }

    return maxDensity * densityScale;
}

#endif