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
#include "headers/RayTracingHelpers.hlsl"
#include "headers/GaussianSplatting.hlsl"
#include "headers/VolumeGridLib.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/RenderableResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/GaussianRadianceFieldResources.hlsl"

struct TraceTransmittanceRaysUB {
    uint Seed;
    uint3 Padding;
};
ConstantBuffer<TraceTransmittanceRaysUB> UB;

RaytracingAccelerationStructure TLAS;

StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<VolumePrimitivesHeader> VolumePrimitivesHeaderBuffer;
StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
StructuredBuffer<VolumeGridHeader> VolumeGridHeaderBuffer;


Texture2D<float> G_Depth;
Texture2D<uint> G_GeometryNormal;

StructuredBuffer<uint> RayToTraceListLengthBuffer;
StructuredBuffer<uint> RayToTraceListBuffer;

StructuredBuffer<float3> RayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;
RWStructuredBuffer<float> RWRayToTraceTransmittanceBuffer;

// Optional (when the starting point is exactly on a pixel center)
StructuredBuffer<uint> RayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
StructuredBuffer<float3> RayToTraceOriginBuffer;

// Optional (when the ray tmax is passed as a parameter, otherwise defaults to far plane)
StructuredBuffer<float> RayToTraceTMaxBuffer; 

#include "headers/HardwareRayTracing.hlsl"

struct [raypayload] RayPayload {
    float HitDistance; // Hit on meshes, TMax for no hits
    float Transmittance; // Transmittance value for the ray
};

[shader("raygeneration")]
void TraceTransmittanceRaysRaygen() {
#ifdef USE_RAY_LIST
    uint RayListIndex = DispatchRaysIndex().x;
    uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
    uint RayIndex = DispatchRaysIndex().x;
#endif

    RayDesc Ray;
    SetupRayDesc(RayIndex, Ray);

    RayPayload Payload = (RayPayload)0;
    Payload.HitDistance = Ray.TMax; // Default to TMax, will only be updated in a closest hit on meshes
    Payload.Transmittance = 1.0f; // Default transmittance value, will be updated in any hits
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
    // Write back transmittance
    RWRayToTraceTransmittanceBuffer[RayIndex] = Payload.Transmittance;
}

[shader("miss")]
void TraceTransmittanceRaysMiss(inout RayPayload Payload: SV_RayPayload) {
    // ...
}

[shader("anyhit")]
void TraceTransmittanceRaysAnyHit(inout RayPayload Payload: SV_RayPayload,
                                   BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_NONE) {
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
        // Semi-transparent surfaces
        Payload.Transmittance *= saturate(1.f - ColorOpacity.a);
	    if(ColorOpacity.a < 0.99f) {
 		    IgnoreHit();
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
            // Multiply to transmittance
            float Transmittance = exp(-Length * Opacity);
            Payload.Transmittance *= Transmittance;
            if(Payload.Transmittance < 0.001f) {
                // Early termination if transmittance is too small
                AcceptHitAndEndSearch();
            }
        }
        // Always ignore hits on volume primitives
        IgnoreHit();
    } else if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_GAUSSIAN_RADIANCE_FIELD) {
        // 3D gaussian radiance field instance
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the 3d gaussian (each 3d gaussian have 20 triangles for proxy geometry) 
        uint InstanceGaussianIndex = PrimitiveIndex() / 20;
        GaussianRadianceFieldInstanceHeader GRFInstanceHeader = GetGaussianRadianceFieldInstanceHeader(RenderableHeaderBuffer[Instance]);
        uint RadianceFieldIndex = GRFInstanceHeader.FieldIndex;
        uint GaussianOffset = GaussianRadianceFieldHeaderBuffer[RadianceFieldIndex].PointOffset;
        uint GaussianIndex = GaussianOffset + InstanceGaussianIndex;
        Gaussian3D G = UnpackGaussian(Gaussian3DBuffer[GaussianIndex]);
        RayDesc Ray = GetRayDesc();
        float RayScaler = 1, RayT = 0;
        float3x4 WorldToObject = WorldToObject3x4();
        float3x3 WorldToObjectNormal = transpose(To3x3(WorldToObject3x4()));
        float3 LocalRayOrigin = TransformPoint(WorldToObject, Ray.Origin);
        float3 RayTangent, RayBitangent;
        GetOrthoVectors(Ray.Direction, RayTangent, RayBitangent);
        float3 LocalRayDirection = TransformVector(WorldToObject, Ray.Direction);
        float3 LocalRayTangent   = TransformVector(WorldToObjectNormal, RayTangent);
        float3 LocalRayBitangent = TransformVector(WorldToObjectNormal, RayBitangent);
        float3x3 RaySpace = float3x3(
            LocalRayTangent   / dot(LocalRayTangent, LocalRayTangent),
            LocalRayBitangent / dot(LocalRayBitangent, LocalRayBitangent),
            LocalRayDirection / dot(LocalRayDirection, LocalRayDirection)
        );
        float Alpha = 
            EvaluateGaussianResponseRast(LocalRayOrigin, RaySpace, G, RayT);
        Payload.Transmittance *= Alpha;
        if(Payload.Transmittance < 0.001f) {
            // Early termination if transmittance is too small
            AcceptHitAndEndSearch();
        }
        // Always ignore hits on gaussian RF
        IgnoreHit();
    } else if (InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_GRID) {
        // 1. 获取 Header 和 Instance 数据
        VolumeGridInstanceHeader InstanceHeader = GetVolumeGridInstanceHeader(RenderableHeaderBuffer[Instance]);
        uint GridIndex = InstanceHeader.VolumeGridIndex;
        VolumeGridHeader Grid = VolumeGridHeaderBuffer[GridIndex];

        // 2. 准备光线和空间变换
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        float TMin = RayTMin();
        float TMax = RayTCurrent(); // 对于 AnyHit，TCurrent 是当前交点距离

        // 3. 计算进出点 (AABB Intersection)
        // BLAS 是一个 Unit Cube 或 Local AABB，已经被 Transform 到世界空间了。
        // 但由于我们是在 AnyHit 里，光线已经在 Object Space 做了相交测试。
        // 这里我们可以重新在世界空间算，或者利用 Object Space 的特性。
        // 为了简便和准确，我们直接在世界空间通过 AABB 算进出点（因为 3D Texture 是轴对齐的）

        // 获取 Instance 的 Transform
        float3x4 WorldToObject = WorldToObject3x4();
        float3 localRayOrigin = mul(WorldToObject, float4(RayOrigin, 1.0));
        float3 localRayDir = mul((float3x3)WorldToObject, RayDirection);

        // AABB求交
        float t0, t1;
        IntersectAABB(localRayOrigin, localRayDir, Grid.LocalMin, Grid.LocalMax, t0, t1);
        t0 = max(t0, TMin);
        t1 = min(t1, TMax);

        if (t0 < t1) {
            // 加载体网格纹理
            uint bindlessIndex = Grid.TextureBindlessIndex;
            Texture3D<float4> densityTex = GetBindlessVolumeSRV(bindlessIndex);

            // 准备将光线转换到[0,1]的纹理UVW空间
            float3 boxSize = Grid.LocalMax - Grid.LocalMin;
            float3 invBoxSize = 1.0f / boxSize;

            float3 uvwOrigin = (RayOrigin - Grid.LocalMin) * invBoxSize;
            float3 uvwDirection = RayDirection * invBoxSize;

            float densityScale = 1.0f; // May be added in the future.
            float majorant = CalculateMaxDensityDDA(densityTex, uvwOrigin, uvwDirection, t0, t1, densityScale);

            // Ratio Tracking
            float t = t0;
            float transmittance = 1.0f;
#ifdef USE_RAY_LIST
            uint RayListIndex = DispatchRaysIndex().x;
            uint RayIndex = RayToTraceListBuffer[RayListIndex];
#else
            uint RayIndex = DispatchRaysIndex().x;
#endif
            Random rng = MakeRandom(RayIndex + 0x8f71a213u, UB.Seed);

            // Ratio Tracking Loop
            while (true) {
                t -= log(1.0f - rng.rand()) / majorant;
                if (t >= t1) break;

                float3 pos = localRayOrigin + localRayDir * t;
                float3 uvw = (pos - Grid.LocalMin) / boxSize;

                // 采样密度
                float density = densityTex.SampleLevel(LinearWrapSampler, uvw, 0).a * densityScale;

                // Null-collision 概率
                float nullProb = 1.0f - (density / majorant);
                transmittance *= nullProb;

                // 如果透射率太低，提前终止
                if (transmittance < 0.001f) {
                    transmittance = 0.0f;
                    break;
                }
            }

            Payload.Transmittance *= transmittance;
            if (Payload.Transmittance < 0.001f) {
                AcceptHitAndEndSearch();
            }
        }

        // Volume 总是透光的 (除非 Ratio Tracking 归零)，所以忽略这个几何 Hit，让光线继续
        IgnoreHit();

    } else {
        // Always ignore hits on unknown instance types
        IgnoreHit();
    }
}

[shader("closesthit")]
void TraceTransmittanceRaysClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    if(InstanceFlags == INSTANCE_CUSTOM_INDEX_FLAG_NONE) {
        // Found a static mesh instance. Return the hit distance.
        Payload.HitDistance = RayTCurrent();
        Payload.Transmittance = 0;
    } else {
        // Otherwise the ray is early terminated in anyhit shader.
        // Simply set the transmittance to 0 and return.
        Payload.Transmittance = 0;
    }
}
