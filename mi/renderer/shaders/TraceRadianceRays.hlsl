#include "headers/Conventions.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "shared/SharedVertex.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "headers/Random.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/MaterialResources.hlsl"
#include "resources/EnvironmentLightResource.hlsl"

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

struct RayPayload {
    float HitDistance; // Hit on meshes, TMax for no hits
    float3 Radiance; // Radiance value for the ray
    float U; // Random number
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
    } else {
        float3 RayOrigin = WorldRayOrigin();
        float3 RayDirection = WorldRayDirection();
        // Get the index of the volume primitive (each volume primitive have 20 triangles for proxy geometry) 
        uint InstancePrimitiveIndex = PrimitiveIndex() / 20;
        uint PrimitiveOffset = VolumePrimitivesHeaderBuffer[Instance].PrimitiveOffset;
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
    }
}

[shader("closesthit")]
void TraceRadianceRaysClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle          = PrimitiveIndex();
    uint DescriptionIndex  = GeometryIndex();
    uint InstanceCustomIndex = InstanceID(); // Custom instance ID, not the instance index in the TLAS
    uint InstanceFlags = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_FLAGS_MASK;
    uint Instance = InstanceCustomIndex & INSTANCE_CUSTOM_INDEX_INDEX_MASK;
    Payload.HitDistance = RayTCurrent();
    Payload.Radiance = 0;
    // TODO
}