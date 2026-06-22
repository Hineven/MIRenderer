#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "headers/Random.hlsl"
#include "headers/VertexShaderInstanceIndex.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"
#include "resources/GigaVoxelResources.hlsl"

struct DrawDeferredStaticMeshesShaderUB {
    float FragmentOpaqueThreshold;
    uint32_t Padding0, Padding1, Padding2; // Pad to 16 bytes
};

ConstantBuffer<DrawDeferredStaticMeshesShaderUB> UB;

StructuredBuffer<uint2>            RenderableIndexAndDescriptorIndexBuffer;

struct DrawDeferredStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    // x-channel visibility content: RenderableIndex(20) | RenderableType(12)<<20.
    uint   RenderableIndexType : TEXCOORD0;
    uint   DescriptorIndex : TEXCOORD1; // Geometry descriptor index within the mesh
    uint   MaterialIndex : TEXCOORD2;
    float2 UV : TEXCOORD3;
};

// TODO provide an optimized path for opaque materials
DrawDeferredStaticMeshesVSOut DrawDeferredStaticMeshesVS (
    DefaultStaticMeshVertex Vertex,
    VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS
) {
    uint InstanceIndex = VS_INSTANCE_INDEX;
    uint2 RenderableIndex_DescriptorIndex = RenderableIndexAndDescriptorIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableIndex_DescriptorIndex.x;
    uint DescriptorIndex = RenderableIndex_DescriptorIndex.y;
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));
    
    uint StaticMeshIndex = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).StaticMeshIndex;
    StaticMeshHeader StaticMeshHeader = StaticMeshHeaderBuffer[StaticMeshIndex];
    // Compute the global descriptor index
    uint  GlobalDescriptorIndex = StaticMeshHeader.DescriptionOffset + DescriptorIndex;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[GlobalDescriptorIndex];

    DrawDeferredStaticMeshesVSOut Output = (DrawDeferredStaticMeshesVSOut)0;
    Output.Position = PositionW;
    // Visibility x-channel: RenderableIndex(20) | RenderableType(12)<<20.
    Output.RenderableIndexType = (RenderableIndex & 0xFFFFFu) | (MI_RENDERABLE_TYPE_StaticMesh << 20);
    Output.DescriptorIndex = DescriptorIndex;
    Output.MaterialIndex = GeometryMaterialPair.y;
    // Here we use the faster path to interpolate UVs rather than decoding full visibility in fragment shader.
    Output.UV = Vertex.UV;
    return Output;
}


struct DrawDeferredStaticMeshesPSOut {
    uint4 Visibility : SV_TARGET0;
};

DrawDeferredStaticMeshesPSOut DrawDeferredStaticMeshesPS (
    DrawDeferredStaticMeshesVSOut Input,
    uint PrimitiveIndex : SV_PrimitiveID,
    float3 Barycentrics : SV_BaryCentrics
) {
    DrawDeferredStaticMeshesPSOut Output = (DrawDeferredStaticMeshesPSOut)0;
    MaterialHeader Material = MaterialHeaderBuffer[Input.MaterialIndex];
    if(0 == (Material.Flags & MATERIAL_FLAG_OPAQUE)) {
        float4 ColorOpacity = float4(Material.Albedo, 1);
        if(IsValid(Material.AlbedoMap)) {
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).Sample(LinearWrapSampler, Input.UV);
        }
        // stochastic alpha test
        float MinAlpha = 0.3f;
        float MaxAlpha = 0.7f;
        CameraParameters C = GetActiveCamera();
        float2 NoiseUV = float2(Input.UV.x * C.FilmAspectRatioAndInvAspectRatio.x, Input.UV.y);
        float Threshold = lerp(MinAlpha, MaxAlpha, InterleavedGradientNoise(NoiseUV * 259, GetViewFrameIndex()));
        if(ColorOpacity.a < max(Threshold, UB.FragmentOpaqueThreshold)) {
            // Discard the pixel
            discard;
        }
    }
    // Visibility payload (StaticMesh, per gigavoxel_visibility_buffer.md §3.1):
    //   x[19:0] RenderableIndex, x[31:20] RenderableType (= kStaticMesh)
    //   y[7:0]  DescriptorIndex, y[31:8] PrimitiveIndex
    //   z = Bary.y, w = Bary.z
    Output.Visibility = uint4(
        Input.RenderableIndexType,
        (Input.DescriptorIndex & 0xFFu) | ((PrimitiveIndex & 0xFFFFFFu) << 8),
        asuint(Barycentrics.y),
        asuint(Barycentrics.z)
    );
    return Output;
}

[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWAlbedo;
[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWNormal;
[[vk::image_format("r32ui")]]
RWTexture2D<uint> RWGeometryNormal;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWEmission;
[[vk::image_format("rg8")]]
RWTexture2D<float2> RWMetallicRoughness;
[[vk::image_format("rg32f")]]
RWTexture2D<float2> RWMotionVector;
Texture2D<uint4> VisibilityTexture;
Texture2D<float> DepthTexture;

[numthreads(1, 1, 1)]
void Test() {
    RWAlbedo[uint2(0,0)] = float4(1,0,0,1);
}

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

// Shared G-buffer writer for the decode pass: takes a resolved intersection +
// per-pixel view direction (for face-flip robustness) + motion vector, and
// writes all 6 G-buffer channels. Empty pixels are the caller's responsibility.
void DecodeWriteIntersectionToGBuffers(uint2 PixelCoords,
                                       IntersectionMaterial Intersection,
                                       float3 ViewDirection,
                                       float2 Motion) {
    // 25.10.21: Alpha should always be 1.
    RWAlbedo[PixelCoords] = float4(Intersection.Albedo, 1.f);
    float3 OutShadingNormal = Intersection.ShadingNormal;
    float3 OutGeometryNormal = Intersection.GeometryNormal;
    // Check for face flipping for all materials for maximum robustness.
    {
        bool bFaceFlipped = dot(Intersection.GeometryNormal, ViewDirection) < 0;
        if (bFaceFlipped) {
            OutShadingNormal = -Intersection.ShadingNormal;
            OutGeometryNormal = -Intersection.GeometryNormal;
        }
    }
    // Squash normal to [0,1]
    float3 GBufferShadingNormal = (OutShadingNormal * 0.5f) + 0.5f;
    RWNormal[PixelCoords] = float4(GBufferShadingNormal, 1);
    RWGeometryNormal[PixelCoords] = PackGeometryNormal(OutGeometryNormal);
    RWEmission[PixelCoords] = float4(Intersection.Emission, 1);
    RWMetallicRoughness[PixelCoords] = Intersection.MetallicRoughness;
    RWMotionVector[PixelCoords] = Motion;
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void DecodeVisibility (uint2 DispatchID : SV_DispatchThreadID) {
    uint2 PixelCoords = DispatchID;
    if(any(PixelCoords >= GetActiveCamera().FilmDimensions)) return;
    float Depth = DepthTexture.Load(uint3(PixelCoords, 0)).x;
    if(Depth == 0) {
        // Empty pixel (reversed-z: 0 = far plane = never written)
        return;
    }
    uint4 Visibility = VisibilityTexture.Load(uint3(PixelCoords, 0));
    // Decode visibility (per gigavoxel_visibility_buffer.md §2.1):
    //   x[19:0] RenderableIndex, x[31:20] RenderableType
    //   payload (y/z/w) interpreted per RenderableType.
    uint RenderableIndex = Visibility.x & 0xFFFFFu;
    uint RenderableType  = (Visibility.x >> 20) & 0xFFFu;

    IntersectionMaterial Intersection = (IntersectionMaterial)0;
    float3 LocalPosition = 0;

    if (RenderableType == MI_RENDERABLE_TYPE_StaticMesh) {
        // StaticMesh payload: y[7:0] DescriptorIndex, y[31:8] PrimitiveIndex; z/w = Bary.y/.z
        float2 Barycentrics = asfloat(Visibility.zw);
        uint PrimitiveIndex = Visibility.y >> 8;
        uint DescriptorRank = Visibility.y & 0xFFu;
        Intersection = EvaluateStaticMeshRenderableIntersectionMaterial(
            RenderableIndex, DescriptorRank, PrimitiveIndex, Barycentrics, 0);
        LocalPosition = Intersection.LocalPosition;
    } else if (RenderableType == MI_RENDERABLE_TYPE_GigaVoxel) {
        // GigaVoxel VC payload (per DrawGigaVoxel.hlsl PS encoding):
        //   y      = GlobalChunkIndex
        //   z[15:0]= PrimitiveIndex (chunk-local), z[31:16] = f16(Bary.y)
        //   w[15:0]= f16(Bary.z), w[31:16] = 0 (reserved; NaN sentinel for LFC)
        // Bary.x = 1 - Bary.y - Bary.z (derived).
        uint GlobalChunkIndex = Visibility.y;
        uint PrimitiveIndex = Visibility.z & 0xFFFFu;
        float BaryY = f16tof32(Visibility.z >> 16);
        float BaryZ = f16tof32(Visibility.w & 0xFFFFu);
        float2 Barycentrics = float2(BaryY, BaryZ);
        // Vertices are chunk-local: build the chunk-local -> world transform from
        // the chunk header's ChunkOrigin (a pure translation; the raster path has
        // no ObjectToWorld3x4() builtin).
        float3 ChunkOrigin = GigaVoxelChunkHeaderBuffer[GlobalChunkIndex].ChunkOrigin;
        float3x4 ChunkToWorld = float3x4(
            1, 0, 0, ChunkOrigin.x,
            0, 1, 0, ChunkOrigin.y,
            0, 0, 1, ChunkOrigin.z);
        Intersection = EvaluateGigaVoxelVisibilityIntersectionMaterial(
            RenderableIndex, GlobalChunkIndex, PrimitiveIndex, Barycentrics, ChunkToWorld);
        // GigaVoxel terrain is static: its RenderableTransform (instance-level,
        // PrevRenderableTransformBuffer[RenderableIndex]) is identity, and chunk
        // origins don't move between frames. So the previous-frame world position
        // equals the current one — feed the world position into LocalPosition so
        // the shared motion-vector math (PrevToWorld * LocalPosition) lands on it.
        LocalPosition = Intersection.WorldPosition;
    } else {
        // Unknown / deprecated type: leave empty.
        return;
    }

    // Motion vector + view direction (shared), then G-buffer write.
    CameraParameters C = GetActiveCamera();
    uint CurrHash = RenderableHashBuffer[RenderableIndex];
    uint PrevHash = PrevRenderableHashBuffer[RenderableIndex];
    bool ValidHistory = CurrHash == PrevHash;
    float3x4 PrevToWorld = PrevRenderableTransformBuffer[RenderableIndex];
    float3 PrevWorldPos = mul(PrevToWorld, float4(LocalPosition, 1));
    float4 PrevClip = mul(View.PreviousCamera.WorldToNDC, float4(PrevWorldPos, 1));
    float2 PrevNDC = PrevClip.xy / max(PrevClip.w, 1e-8f);
    PrevNDC.xy -= C.PrevJitter;
    float4 CurrClip = mul(View.Camera.WorldToNDC, float4(Intersection.WorldPosition, 1));
    float2 CurrNDC = CurrClip.xy / max(CurrClip.w, 1e-8f);
    CurrNDC.xy -= C.Jitter;
    float2 Motion = ValidHistory ? (CurrNDC - PrevNDC) : 0;
    float3 ViewDirection = -NDC2ToCameraDirection(C, CurrNDC.xy);

    DecodeWriteIntersectionToGBuffers(PixelCoords, Intersection, ViewDirection, Motion);
}

struct DrawForwardStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    uint   RenderableIndexType : TEXCOORD0; // RenderableIndex(20) | RenderableType(12)<<20
    uint   DescriptorIndex : TEXCOORD1;
    uint   MaterialIndex : TEXCOORD2;
    float2 UV : TEXCOORD3;
};

DrawForwardStaticMeshesVSOut DrawForwardStaticMeshesVS (
    DefaultStaticMeshVertex Vertex,
    VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS
) {
    uint InstanceIndex = VS_INSTANCE_INDEX;
    uint2 RenderableIndex_DescriptorIndex = RenderableIndexAndDescriptorIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableIndex_DescriptorIndex.x;
    uint DescriptorIndex = RenderableIndex_DescriptorIndex.y;
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));

    DrawForwardStaticMeshesVSOut Output = (DrawForwardStaticMeshesVSOut)0;
    Output.Position = PositionW;
    Output.RenderableIndexType = (RenderableIndex & 0xFFFFFu) | (MI_RENDERABLE_TYPE_StaticMesh << 20);
    Output.DescriptorIndex = DescriptorIndex;
    uint StaticMeshIndex = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).StaticMeshIndex;
    StaticMeshHeader StaticMeshHeader = StaticMeshHeaderBuffer[StaticMeshIndex];
    // Compute the global descriptor index
    uint  GlobalDescriptorIndex = StaticMeshHeader.DescriptionOffset + DescriptorIndex;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[GlobalDescriptorIndex];
    Output.MaterialIndex = GeometryMaterialPair.y;

    // Here we use the faster path to interpolate UVs rather than decoding full visibility in fragment shader.
    Output.UV = Vertex.UV;
    return Output;
}


struct DrawForwardStaticMeshesPSOut {
    uint4 Visibility : SV_TARGET0; // Forward visibility (new unified x format)
    float4 ColorAlpha: SV_TARGET1;
};

DrawForwardStaticMeshesPSOut DrawForwardStaticMeshesPS (
    DrawForwardStaticMeshesVSOut Input,
    uint PrimitiveIndex : SV_PrimitiveID,
    float2 Barycentrics : SV_Barycentrics
) {
    DrawForwardStaticMeshesPSOut Output = (DrawForwardStaticMeshesPSOut)0;
    // Same unified payload format as the deferred path (§3.1).
    Output.Visibility = uint4(
        Input.RenderableIndexType,
        (Input.DescriptorIndex & 0xFFu) | ((PrimitiveIndex & 0xFFFFFFu) << 8),
        asuint(Barycentrics.x),
        asuint(Barycentrics.y)
    );

    MaterialHeader Material = MaterialHeaderBuffer[Input.MaterialIndex];
    float4 ColorOpacity = float4(Material.Albedo, 1);
    if(IsValid(Material.AlbedoMap)) {
        ColorOpacity = GetBindlessSRV(Material.AlbedoMap).Sample(LinearWrapSampler, Input.UV);
    }

    Output.ColorAlpha = ColorOpacity;
    return Output;
}
