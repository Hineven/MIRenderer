#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedVolumeGrid.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "headers/Random.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/EnvironmentLightResource.hlsl"
#include "resources/GaussianRadianceFieldResources.hlsl"

struct TraceRadianceRaysUB {
    uint Seed;
    uint3 Padding;
};
ConstantBuffer<TraceRadianceRaysUB> UB;

RaytracingAccelerationStructure TLAS;

StructuredBuffer<RenderableHeader> RenderableHeaderBuffer;
StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;
StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
StructuredBuffer<VolumeGridHeader> VolumeGridHeaderBuffer;


Texture2D<float> G_Depth;

StructuredBuffer<uint> RayToTraceListLengthBuffer;
StructuredBuffer<uint> RayToTraceListBuffer;

StructuredBuffer<float3> RayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;
RWStructuredBuffer<uint2> RWRayToTraceResultBuffer;

// Optional (when the starting point is exactly on a pixel center)
StructuredBuffer<uint> RayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
StructuredBuffer<float3> RayToTraceOriginBuffer;

// Optional (when the ray tmax is passed as a parameter, otherwise defaults to far plane)
StructuredBuffer<float> RayToTraceTMaxBuffer; 


struct [raypayload] RayPayload {
    float HitDistance; // Hit on meshes, TMax for no hits
    float3 Radiance; // Radiance value for the ray
    float U; // Random number
    // [新增] 用于多跳和 Volume 状态
    float3 Throughput;
    uint   Depth;
    uint   IsVolumeHit; // 标记这是 Surface Hit 还是 Volume Scatter Hit
};

[shader("raygeneration")]
void TraceRadianceRaysRaygen() {
#ifdef USE_RAY_LIST
    uint RayListIndex = DispatchRaysIndex().x;
    uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
    uint RayIndex = DispatchRaysIndex().x;
#endif

    RayDesc Ray = (RayDesc)0;
    {
        CameraParameters C = GetActiveCamera();
#ifndef USE_SCREEN_COORDS
        Ray.Origin = RayToTraceOriginBuffer[RayIndex];
#else 
        uint2 PixelIndex = UnpackUint2x16(RayToTraceOriginScreenCoordBuffer[RayIndex]);
        float2 UV = (PixelIndex + 0.5f) * C.InvFilmDimensions;
        float ReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, UV, 0);
        float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
        Ray.Origin = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
#endif
        Ray.Direction = RayToTraceDirectionBuffer[RayIndex];
        bool bHit = false;
        Ray.TMin = UnpackRayToTraceState(RWRayToTraceStateBuffer[RayIndex], bHit);
#ifdef USE_RAY_TMAX_BUFFER
        Ray.TMax = RayToTraceTMaxBuffer[RayIndex];
#else
        Ray.TMax = C.FarPlane;
#endif
    }

    RayPayload Payload = (RayPayload)0;
    Payload.HitDistance = Ray.TMax; // Default to TMax, will only be updated in a closest hit on meshes
    Payload.Radiance = 0.f;
    Random rng = MakeRandom(RayIndex + 0x8f71a213u, UB.Seed);
    Payload.U = rng.rand();
    Payload.Throughput = 1.f;
    Payload.Depth = 0;
    Payload.IsVolumeHit = 0;
    TraceRay(
        TLAS,
        // Proxy geometries of the volume primitives are built face-flipped. So culling back faces
        // means culling real front faces for volume primitives. Only real back faces from volume
        // primitives are hit when tracing the ray.
        RAY_FLAG_CULL_BACK_FACING_TRIANGLES,
        0xFF, // Ray mask
        0,    // SBT offset
        0,    // SBT stride
        0,    // Miss shader index
        Ray,
        Payload
    );
    uint PackedTraceResult = PackRayToTraceState(Payload.HitDistance, Payload.HitDistance < Ray.TMax);
    RWRayToTraceStateBuffer[RayIndex] = PackedTraceResult;
    // Write back Radiance
    RWRayToTraceResultBuffer[RayIndex] = PackFp16x4Safe(float4(Payload.Radiance, 0));
}

[shader("miss")]
void TraceRadianceRaysMiss(inout RayPayload Payload: SV_RayPayload) {
    float3 RayDirection = WorldRayDirection();
    float3 EnvironmentColor = EvaluateEnvironmentMap(-RayDirection);
    Payload.Radiance = EnvironmentColor;
}

[shader("anyhit")]
void TraceRadianceRaysAnyHit(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    if(InstanceFlags == 0) {
        // Static mesh instance
        StaticMeshInstanceHeader InstanceHeader = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[Instance]);
        uint StaticMeshIndex = InstanceHeader.StaticMeshIndex;
        uint DescriptionOffset = StaticMeshHeaderBuffer[StaticMeshIndex].DescriptionOffset;
        uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[DescriptionOffset + DescriptionIndex];
        uint GeometryIndex = GeometryMaterialPair.x;
        uint MaterialIndex = GeometryMaterialPair.y;
        GeometryHeader Geometry = GeometryHeaderBuffer[GeometryIndex];
        uint IndexOffset = Geometry.IndexOffset + Triangle * 3;
        uint VertexOffset = Geometry.VertexOffset;

        uint VertexAIndex = VertexOffset + IndexBuffer[IndexOffset + 0];
        uint VertexBIndex = VertexOffset + IndexBuffer[IndexOffset + 1];
        uint VertexCIndex = VertexOffset + IndexBuffer[IndexOffset + 2];
        DefaultStaticMeshVertex VertexA = VertexBuffer[VertexAIndex];
        DefaultStaticMeshVertex VertexB = VertexBuffer[VertexBIndex];
        DefaultStaticMeshVertex VertexC = VertexBuffer[VertexCIndex];

        // Interpolate the vertex
        DefaultStaticMeshVertex InterpolatedVertex = InterpolateVertex(VertexA, VertexB, VertexC, Attributes.barycentrics);

        MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];
        float4 ColorOpacity = float4(Material.Albedo, 1);
        if(IsValid(Material.AlbedoMap)) {
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).SampleLevel(LinearWrapSampler, InterpolatedVertex.UV, 0);
        }

	    if(ColorOpacity.a < Payload.U) {
            Payload.U = (Payload.U - ColorOpacity.a) / (1.f - ColorOpacity.a);
            // Semi-transparent surfaces, continue tracing
 		    IgnoreHit();
	    } else {
            Payload.U = Payload.U / max(ColorOpacity.a, 1e-5f);
        }
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES) {
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the volume primitive (each volume primitive have 20 triangles for proxy geometry) 
        uint InstancePrimitiveIndex = PrimitiveIndex() / 20;
        VolumePrimitivesInstanceHeader InstanceHeader = GetVolumePrimitivesInstanceHeader(RenderableHeaderBuffer[Instance]);
        uint VolprimsIndex = InstanceHeader.VolumePrimitivesIndex;
        uint PrimitiveOffset = VolumePrimitivesHeaderBuffer[VolprimsIndex].PrimitiveOffset;
        uint PrimitiveIndex = PrimitiveOffset + InstancePrimitiveIndex;
        VolumePrimitive Primitive = UnpackVolumePrimitive(PrimitiveData[PrimitiveIndex]);
        float3x4 ToObject = WorldToObject3x4();
        float2 lr = 0;
        float Dist = 0;
        bool bIntersected = RayIntersect(RayOrigin, RayDirection, Primitive, ToObject, lr, Dist);
        if(bIntersected) {
            float TMin = RayTMin();
            lr.x = max(lr.x, TMin);
            lr.y = max(lr.y, TMin);
            float Length = max(lr.y - lr.x, 0);
            float Opacity = Primitive.Opacity * VolumePrimitiveRayDecay(Dist);
            float Transmittance = exp(-Length * Opacity);
            if(Transmittance < Payload.U) {
                // The ray is absorbed in the volume
                // ...assume the hit is on the primitive's proxy backface
                Payload.U = (Payload.U - Transmittance) / (1.f - Transmittance);
            } else {
                Payload.U = Payload.U / max(Transmittance, 1e-5f);
                IgnoreHit();
            }
        }
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_GAUSSIAN_RADIANCE_FIELD) {
        // TODO

/*
    }  else if (InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_GRID) {
        // 1. 准备数据
        VolumeGridInstanceHeader InstanceHeader = GetVolumeGridInstanceHeader(RenderableHeaderBuffer[Instance]);
        uint GridIndex = InstanceHeader.VolumeGridIndex;
        VolumeGridHeader Grid = VolumeGridHeaderBuffer[GridIndex];

        // 2. 空间变换与 AABB Intersection
        float3x4 WorldToObject = WorldToObject3x4();
        float3 localRayOrigin = mul(WorldToObject, float4(WorldRayOrigin(), 1.0));
        float3 localRayDir = mul((float3x3)WorldToObject, WorldRayDirection());

        float t0, t1;
        if (IntersectAABB(localRayOrigin, localRayDir, Grid.LocalMin, Grid.LocalMax, t0, t1)) {
            float TMin = RayTMin();
            // 裁剪光线：必须从 max(t0, TMin) 开始，到 min(t1, HitT) 结束
            float tStart = max(t0, TMin);
            float tEnd = min(t1, RayTCurrent());

            if (tStart < tEnd) {
                uint bindlessIndex = Grid.TextureBindlessIndex;
                Texture3D<float4> densityTex = GetBindlessVolumeSRV(bindlessIndex);

                // 3. Delta Tracking (Free-flight sampling)
                float3 boxSize = Grid.LocalMax - Grid.LocalMin;
                float3 invBoxSize = 1.0f / boxSize;
                float3 uvwOrigin = (localRayOrigin - Grid.LocalMin) * invBoxSize;
                float3 uvwDir = localRayDir * invBoxSize;

                float densityScale = 1.0f; //
                float majorant = CalculateMaxDensityDDA(densityTex, uvwOrigin, uvwDir, tStart, tEnd, densityScale);

#ifdef USE_RAY_LIST
                uint RayListIndex = DispatchRaysIndex().x;
                uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
                uint RayIndex = DispatchRaysIndex().x;
#endif
                Random rng = MakeRandom(RayIndex + 0x8f71a213u, UB.Seed);

                float t = tStart;
                bool scattered = false;

                while (true) {
                    // 步进距离
                    t -= log(1.0f - rng.rand()) / majorant;

                    if (t >= tEnd) {
                        break; // 飞出了体积，没有碰撞
                    }

                    float3 pos = localRayOrigin + localRayDir * t;
                    float3 uvw = (pos - Grid.LocalMin) / boxSize;

                    // 采样真实密度
                    float density = densityTex.SampleLevel(LinearWrapSampler, uvw, 0).a * densityScale;

                    // 接受/拒绝采样
                    if (rng.rand() < (density / majorant)) {
                        scattered = true;
                        break;
                    }
                }

                if (scattered) {
                    // 发生了散射！
                    // 我们接受这个 Hit，并将 HitDistance 更新为散射点
                    // 这样管线会调用 ClosestHit，我们可以在那里做 NEE 和 Phase Function

                    // 更新 Payload 状态，告诉 ClosestHit 这是个 Volume Hit
                    Payload.IsVolumeHit = true;
                    Payload.U = rng.rand(); // 存回 RNG 状态

                    Payload.HitDistance = t; // 这是 Scatter 的 T
                    AcceptHitAndEndSearch();
                }
            }
        }
        // 如果 Delta Tracking 跑完了全程都没 Scatter，就忽略这个盒子，继续往前飞
        IgnoreHit();
*/
    }
}

[shader("closesthit")]
void TraceRadianceRaysClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    /* if (Payload.IsVolumeHit) {
        // --- Volume Scattering Logic ---

        // 1. 重建散射点位置
        float3 rayOrigin = WorldRayOrigin();
        float3 rayDir = WorldRayDirection();
        float3 scatterPos = rayOrigin + rayDir * Payload.HitDistance;

        // 2. 准备 RNG
        Random rng = MakeRandom(Payload.U * 1234, UB.Seed);

        // 3. Direct Lighting (NEE)
        // 采样光源，发射 shadow ray (Transmittance Ray)
        // 假设有一个 SampleLight 函数
        LightSample ls = SampleLight(scatterPos, rng);
        if (ls.pdf > 0) {
            // 发射 shadow ray 检测可见性 (Ratio Tracking)
            float transmittance = TraceTransmittance(scatterPos, ls.direction, ls.distance);

            // Phase Function (Isotropic for smoke)
            float phase = 1.0f / (4.0f * PI);

            // 累积 Radiance
            float3 contribution = ls.radiance * transmittance * phase * Payload.Throughput;
            // 这里的 Payload.Radiance 需要在 RayGen 里累积，或者这里累积
            // 根据代码 TraceRadianceRaysRaygen 来看，它直接写回 Buffer。
            // 我们这里先简单累加
            Payload.Radiance += contribution;
        }

        // 4. Indirect Lighting (Next Bounce)
        // 采样新的方向 (Isotropic Phase Function)
        float3 newDir = SampleUnitSphere(rng);

        // 更新 Payload 以供下一跳使用 (如果 RayGen 支持递归或迭代)
        // 假设 RayGen 有一个循环或者递归 TraceRay
        // 更新 Ray 的原点和方向 (通过 Payload 传回 RayGen 或者 这里的 T)

        // 俄罗斯轮盘赌 (RR)
        float maxThroughput = max(max(Payload.Throughput.x, Payload.Throughput.y), Payload.Throughput.z);
        if (rng.rand() < 0.2f) { // 简单 RR
             // Terminate
             Payload.Throughput = 0;
        } else {
             Payload.Throughput /= 0.8f;
             // 散射系数/概率密度 (Isotropic cancel out)
             // Albedo (Single Scattering Albedo). 假设为 0.9
             Payload.Throughput *= 0.9f;
        }

        // 注意：标准的 DXR ClosestHit 无法直接改变 RayGen 的 Ray 变量。
        // RayGen 需要读取 Payload 并决定下一条光线。
        // 我们需要在 Payload 里增加 NextRayOrigin 和 NextRayDirection
    } else */
    {
        // ... 原有的 Surface Hit 逻辑 ...
        Payload.IsVolumeHit = false;
        Payload.HitDistance = RayTCurrent();
        // ... Surface Shading ...
        uint Triangle          = PrimitiveIndex();
        uint DescriptionIndex  = GeometryIndex();
        uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
        uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
        uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
        Payload.HitDistance = RayTCurrent();
        Payload.Radiance = 0;
        // TODO
    }
}