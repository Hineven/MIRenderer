// GigaVoxel VC chunk visibility-buffer rasterization.
//
// Renders greedy-meshed VC chunk triangles into the shared G_visibility_ +
// G_depth_ targets (kLoad, so it layers on top of the static-mesh deferred
// pass). The PS encodes the unified GigaVoxel VC payload (see
// gigavoxel_visibility_buffer.md §3.2) for the DecodeVisibility pass.
//
// Payload encoding (96bit, y/z/w):
//   y      = GlobalChunkIndex
//   z[15:0]= PrimitiveIndex (chunk-local), z[31:16] = f16(Bary.y)
//   w[15:0]= f16(Bary.z), w[31:16] = 0 (must stay NON-NaN -> VC branch)
//
// Decode lives in DrawStaticMeshes.hlsl::DecodeVisibility (GigaVoxel branch),
// which calls EvaluateGigaVoxelVisibilityIntersectionMaterial.

#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedGigaVoxel.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "headers/Random.hlsl"
#include "headers/VertexShaderInstanceIndex.hlsl"

// NOTE: This shader intentionally does NOT include GigaVoxelResources.hlsl.
// The VC raster PS only encodes the visibility payload (RenderableIndex +
// GlobalChunkIndex + PrimitiveIndex + Bary); material evaluation (atlas
// sampling) happens later in DecodeVisibility (DrawStaticMeshes.hlsl), which
// is where GigaVoxelResources.hlsl's helpers are used. Keeping this shader
// free of that header avoids requiring RenderableHeaderBuffer / atlas / sampler
// bindings that the raster pass doesn't need.

// Per-draw (RenderableIndex, GlobalChunkIndex), indexed by VS instance index
// (analogous to RenderableIndexAndDescriptorIndexBuffer in DrawStaticMeshes).
StructuredBuffer<uint2> RenderableIndexAndChunkIndexBuffer;
// Per-chunk geometry header (one row per chunk). The VS reads ChunkOrigin from
// it to place chunk-local vertices into world space (the raster path has no
// ObjectToWorld3x4() builtin, unlike the RT path).
StructuredBuffer<GigaVoxelChunkHeader> GigaVoxelChunkHeaderBuffer;

struct DrawGigaVoxelVCVSOut {
    float4 Position : SV_POSITION;
    uint   RenderableIndexType : TEXCOORD0; // RenderableIndex(20) | RenderableType(12)<<20
    uint   GlobalChunkIndex : TEXCOORD1;
};

DrawGigaVoxelVCVSOut DrawGigaVoxelVCVS (
    // Only the position attribute is bound (see SHADER_VERTEX_ATTRIBUTE in
    // r_giga_voxel.cpp). normal/uv_base/uv_scale are resolved at decode time
    // from the global GigaVoxelVertex uber buffer, not needed in the raster VS.
    float3 Position SEMANTICS(position),
    VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS
) {
    uint InstanceIndex = VS_INSTANCE_INDEX;
    uint2 RenderableChunk = RenderableIndexAndChunkIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableChunk.x;
    uint GlobalChunkIndex = RenderableChunk.y;

    // Vertices are chunk-local: translate by the chunk's world origin (read from
    // the per-chunk header) to place them in world space before clip transform.
    float3 ChunkOrigin = GigaVoxelChunkHeaderBuffer[GlobalChunkIndex].ChunkOrigin;
    float3 WorldPosition = Position + ChunkOrigin;
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));

    DrawGigaVoxelVCVSOut Output = (DrawGigaVoxelVCVSOut)0;
    Output.Position = PositionW;
    Output.RenderableIndexType = (RenderableIndex & 0xFFFFFu) | (MI_RENDERABLE_TYPE_GigaVoxel << 20);
    Output.GlobalChunkIndex = GlobalChunkIndex;
    return Output;
}

struct DrawGigaVoxelVCPSOut {
    uint4 Visibility : SV_TARGET0;
};

DrawGigaVoxelVCPSOut DrawGigaVoxelVCPS (
    DrawGigaVoxelVCVSOut Input,
    uint PrimitiveIndex : SV_PrimitiveID,
    float3 Barycentrics : SV_BaryCentrics
) {
    DrawGigaVoxelVCPSOut Output = (DrawGigaVoxelVCPSOut)0;
    // Encode payload: y=GlobalChunkIndex; z=(PrimitiveIndex | f16(Bary.y)<<16);
    // w=f16(Bary.z). w high bits MUST stay 0 (non-NaN) to select the VC branch.
    uint BaryYF16 = f32tof16(Barycentrics.y) & 0xFFFFu;
    uint BaryZF16 = f32tof16(Barycentrics.z) & 0xFFFFu;
    Output.Visibility = uint4(
        Input.RenderableIndexType,
        Input.GlobalChunkIndex,
        (PrimitiveIndex & 0xFFFFu) | (BaryYF16 << 16),
        BaryZF16
    );
    return Output;
}
