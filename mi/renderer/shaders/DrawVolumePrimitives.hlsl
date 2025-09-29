#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Packing.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/VolumePrimitivesLib.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "resources/CommonSamplerResources.hlsl"

#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE 128
#endif

struct RenderVolumePrimitivesUB {
    uint2 TileDimensions;
    float ExpandFactor;
    uint NumTiles;
    uint MaxNumPrimitiveInstances;
    uint FrameIndex;
    uint2 Padding;
};

struct CollectVolumePrimitivesUB {
    uint RenderableIndex;
    uint InstancePrimitiveOffset;
    uint InstanceNumPrimitives;
    uint Padding;
};
ConstantBuffer<RenderVolumePrimitivesUB> UB;
ConstantBuffer<CollectVolumePrimitivesUB> UB_Collect;


StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
StructuredBuffer<float3x4> RenderableTransformBuffer;
StructuredBuffer<float3x4> RenderableInverseTransformBuffer;

StructuredBuffer<uint> ActivePrimitiveCount;
RWStructuredBuffer<uint> RWActivePrimitiveCount;
// Stores packed renderable index & primitive index
StructuredBuffer<uint> ActivePrimitiveListBuffer;
RWStructuredBuffer<uint> RWActivePrimitiveListBuffer;

StructuredBuffer<uint> PrimitiveInstanceCount;
RWStructuredBuffer<uint> RWPrimitiveInstanceCount;
// Stores packed renderable index & primitive index
RWStructuredBuffer<uint> RWPrimitiveInstanceListBuffer;
RWStructuredBuffer<uint> RWPrimitiveInstanceListKeyBuffer;
StructuredBuffer<uint> PrimitiveInstanceListSortedBuffer;
StructuredBuffer<uint> PrimitiveInstanceListKeySortedBuffer;

StructuredBuffer<uint> TileInstanceOffsetBuffer;
RWStructuredBuffer<uint> RWTileInstanceOffsetBuffer;
StructuredBuffer<uint> TileInstanceCountBuffer;
RWStructuredBuffer<uint> RWTileInstanceCountBuffer;

// Final output
[[vk::image_format("r32f")]]
RWTexture2DArray<float> RWVolumeDensity;
[[vk::image_format("rg16f")]]
RWTexture2D<float2> RWVolumeMinMax;
[[vk::image_format("rgba8")]]
RWTexture2DArray<float4> RWVolumeColor;
[[vk::image_format("rg16f")]]
RWTexture2D<float2> RWVolumeCdfAttenuation;

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeSampleColorAndLinearDepth;
[[vk::image_format("rg16f")]]
RWTexture2D<float2> RWVolumeSampleTransmittanceAndPdf;
[[vk::image_format("r8")]]
RWTexture2D<float>  RWTransmittance;

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void VolumePrimitivesClearCounters(uint DispatchID: SV_DispatchThreadID) {
    if (DispatchID == 0) {
        RWActivePrimitiveCount[0] = 0;
        RWPrimitiveInstanceCount[0] = 0;
    }
    if(DispatchID < UB.NumTiles) {
        RWTileInstanceOffsetBuffer[DispatchID] = 0;
        RWTileInstanceCountBuffer[DispatchID] = 0;
    }
}

VolumePrimitive LoadVolumePrimitive(uint Index) {
    PackedVolumePrimitive PackedPrimitive = PrimitiveData[Index];
    VolumePrimitive Primitive = UnpackVolumePrimitive(PackedPrimitive);
    return Primitive;
}

uint PackRenderablePrimitiveIndex (uint RenderableIndex, uint PrimitiveIndex) {
    // 12 bits for instance index, 20 bits for primitive index
    return (RenderableIndex << 20) | PrimitiveIndex;
}

void UnpackRenderablePrimitiveIndex(uint PackedIndex, out uint RenderableIndex, out uint PrimitiveIndex) {
    RenderableIndex = PackedIndex >> 20;
    PrimitiveIndex = PackedIndex & ((1u << 20) - 1u);
}

groupshared uint SharedGroupActivePrimitiveCount;
groupshared uint SharedListOffset;
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void CollectVolumePrimitives (
    uint LocalID: SV_GroupThreadID,
    uint DispatchID: SV_DispatchThreadID
) {
    // Simply cull primitives whose centers are too far outside of the view frustum.
    if (DispatchID >= UB_Collect.InstanceNumPrimitives) return;
    uint PrimitiveIndex = DispatchID + UB_Collect.InstancePrimitiveOffset;
    VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveIndex);
    float3 Center = Primitive.Position;
    float3 Scales = Primitive.Scales;
    float3x4 ObjectToWorld = RenderableTransformBuffer[UB_Collect.RenderableIndex];
    float4 WorldCenterW = float4(mul(ObjectToWorld, float4(Center, 1)), 1);
    float4 NDCCenterW = mul(GetActiveCamera().WorldToNDC, WorldCenterW);
    float3 NDCCenter = NDCCenterW.xyz / NDCCenterW.w;
    // Check if the center is within the view frustum
    uint bIsActive = 1;
    if (NDCCenterW.w < 0 || any(NDCCenter.xy < -1.3) || any(NDCCenter.xy > 1.3) || NDCCenter.z < 0.01 || NDCCenter.z > 1) {
        // Outside of the view frustum, skip this primitive
        bIsActive = 0;
    }
    float Volume = Scales.x * Scales.y * Scales.z;
    if (Volume < 1e-9f) {
        // Skip degenerate primitives (too small)
        bIsActive = 0;
    }
    if (Primitive.Opacity <= 0.001) {
        // Skip transparent primitives
        bIsActive = 0;
    }
    if(LocalID == 0) {
        // Reset the group active primitive count
        SharedGroupActivePrimitiveCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint GroupActivePrimitiveListOffset = 0, ListOffset = 0;
    InterlockedAdd(SharedGroupActivePrimitiveCount, bIsActive, GroupActivePrimitiveListOffset);
    GroupMemoryBarrierWithGroupSync();
    if (LocalID == 0) {
        uint GroupActiveCount = SharedGroupActivePrimitiveCount;
        InterlockedAdd(RWActivePrimitiveCount[0], GroupActiveCount, ListOffset);
        SharedListOffset = ListOffset;
    }
    GroupMemoryBarrierWithGroupSync();
    ListOffset = SharedListOffset;
    if (bIsActive != 0) {
        // Store the primitive index in the active primitive list
        RWActivePrimitiveListBuffer[ListOffset + GroupActivePrimitiveListOffset] = PackRenderablePrimitiveIndex(UB_Collect.RenderableIndex, PrimitiveIndex);
    }
}

uint PackSortKey(uint TileIndex, uint QuantizedDepth) {
    uint SortKey = (TileIndex << 19) | QuantizedDepth;
    return SortKey;
}

void UnpackSortKey(uint SortKey, out uint TileIndex, out uint QuantizedDepth) {
    TileIndex = SortKey >> 19;
    QuantizedDepth = SortKey & ((1u << 19) - 1u);
}

float3 GetCovariance2DMatrix(float3x4 ToWorldTransform, VolumePrimitive Primitive, out float4 ClipPosW, out uint QuantizedDepth, uint DispatchID) {
    // 1. Transform center to view and clip space, calculate depth
    float4 WorldPosW = float4(TransformPoint(ToWorldTransform, Primitive.Position), 1.0f);
    CameraParameters C = GetActiveCamera();
    float4 ViewPosW = mul(C.WorldToView, WorldPosW);
    ClipPosW = mul(C.WorldToNDC, WorldPosW);

    // Depth is guaranteed to be in [0,1] for centers of active primitives, as per user spec (0.1 to 1).
    // And ClipPosW.w > 0 is also implied.
    float Depth01 = ClipPosW.z / ClipPosW.w;
    QuantizedDepth = (uint)(clamp(Depth01, 0.0f, 1.0f) * ((1u << 19) - 1u)); // Clamp for safety

    // 2. EWA-like projection: Compute 2D screen-space covariance matrix (Sigma_2D)
    float tan_fov_y_half = C.TanFoVY_2;
    float f_y_screen = (0.5f * C.FilmDimensions.y) / tan_fov_y_half;
    float tan_fov_x_half = tan_fov_y_half * C.FilmAspectRatioAndInvAspectRatio.x;
    float f_x_screen = (0.5f * C.FilmDimensions.x) / tan_fov_x_half;

    float xc = ViewPosW.x;
    float yc = ViewPosW.y;
    float zc = -ViewPosW.z;

    // Assume that the positive z-axis is aligned with view direction (left-handed view-space)
    if (zc <= 0.001f) // Primitive too close or behind camera (extra safety, though FilterActivePrimitives should handle)
    {
        return 0.f;
    }

    float inv_zc = 1.0f / zc;
    float inv_zc_sq = inv_zc * inv_zc;

    float2x3 J;
    J[0] = float3(f_x_screen * inv_zc, 0.0f, -f_x_screen * xc * inv_zc_sq);
    J[1] = float3(0.0f, f_y_screen * inv_zc, -f_y_screen * yc * inv_zc_sq);

    float3x3 Rot_s = BuildRotationMatrix(Primitive.Rotation);
    float3x3 Scales_sq_diag = (float3x3)0;
    Scales_sq_diag[0][0] = Primitive.Scales.x * Primitive.Scales.x;
    Scales_sq_diag[1][1] = Primitive.Scales.y * Primitive.Scales.y;
    Scales_sq_diag[2][2] = Primitive.Scales.z * Primitive.Scales.z;

    float3x3 Sigma_local_3D = mul(mul(Rot_s, Scales_sq_diag), transpose(Rot_s));
    float3x3 ToWorld3x3 = To3x3(ToWorldTransform);
    float3x3 Sigma_world_3D = mul(mul(ToWorld3x3, Sigma_local_3D), transpose(ToWorld3x3));

    float3x3 V_rot = float3x3(C.WorldToView._m00, C.WorldToView._m01, C.WorldToView._m02,
                              C.WorldToView._m10, C.WorldToView._m11, C.WorldToView._m12,
                              C.WorldToView._m20, C.WorldToView._m21, C.WorldToView._m22);
    float3x3 Sigma_view_3D = mul(mul(V_rot, Sigma_world_3D), transpose(V_rot));

    float2x2 Sigma_screen_2D = mul(mul(J, Sigma_view_3D), transpose(J));

    // Covariance matrix elements: S = [[s00, s01], [s01, s11]]
    float s00 = Sigma_screen_2D[0][0]; // Corresponds to variance in x if axes aligned
    float s01 = Sigma_screen_2D[0][1]; // Corresponds to covariance xy
    float s11 = Sigma_screen_2D[1][1]; // Corresponds to variance in y if axes aligned
    return float3(s00, s01, s11);
}

groupshared uint SharedNumSortKeysCompacted;
groupshared uint SharedGlobalWriteOffset;

#define K_SQ_ELLIPSE_BOUNDARY 1

[shader("compute")]
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ProjectVolumePrimitives (
    uint DispatchID: SV_DispatchThreadID,
    uint LocalID: SV_GroupThreadID
) {
    // This shader is used to precompute necessary data for the active primitives.
    // It projects active 3D primitives to 2D, determines tile overlap,
    // and generates sort keys (tile_id, depth) and other instance data.

    // Each thread processes one active primitive from the input list.
    // active_primitive_count[0] stores the number of active primitives from FilterActivePrimitives pass.
    uint ActivePrimitiveIndex = DispatchID;
    if (ActivePrimitiveIndex >= ActivePrimitiveCount[0])
    {
        return;
    }

    uint RenderableIndex, PrimitiveIndex;
    uint RenderablePrimitiveIndex = ActivePrimitiveListBuffer[ActivePrimitiveIndex];
    UnpackRenderablePrimitiveIndex(RenderablePrimitiveIndex, RenderableIndex, PrimitiveIndex);
    VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveIndex);

    CameraParameters C = GetActiveCamera();
    float4 ClipPosW; uint QuantizedDepth;

    float3 Covariance2D = GetCovariance2DMatrix(RenderableTransformBuffer[RenderableIndex], Primitive, ClipPosW, QuantizedDepth, DispatchID);

    // 3. Determine screen space bounding box of the projected 2D ellipse
    // For uniform distributions, K_SQ_ELLIPSE_BOUNDARY == 1
    float RadiusXApproax = sqrt(K_SQ_ELLIPSE_BOUNDARY * max(0.0f, Covariance2D.x));
    float RadiusYApproax = sqrt(K_SQ_ELLIPSE_BOUNDARY * max(0.0f, Covariance2D.z));

    float ndc_center_x = ClipPosW.x / ClipPosW.w;
    float ndc_center_y = ClipPosW.y / ClipPosW.w;

    float screen_center_x = (ndc_center_x * 0.5f + 0.5f) * float(C.FilmDimensions.x);
    float screen_center_y = ((1.0f - ndc_center_y) * 0.5f) * float(C.FilmDimensions.y);
    float2 ellipse_screen_center = float2(screen_center_x, screen_center_y);

    // if(DispatchID == 0) {
    //     printf("Approax: %f %f\n", RadiusXApproax, RadiusYApproax);
    // }

    float min_sx_bb = screen_center_x - RadiusXApproax;
    float max_sx_bb = screen_center_x + RadiusXApproax;
    float min_sy_bb = screen_center_y - RadiusYApproax;
    float max_sy_bb = screen_center_y + RadiusYApproax;

    // 4. Determine overlapping tiles based on the bounding box

    int tile_min_x = clamp((int)floor(max(0.0f, min_sx_bb) / 16.0f), 0, UB.TileDimensions.x - 1);
    int tile_max_x = clamp((int)floor(min(float(C.FilmDimensions.x) - 1.0f, max_sx_bb) / 16.0f), 0, UB.TileDimensions.x - 1);
    int tile_min_y = clamp((int)floor(max(0.0f, min_sy_bb) / 16.0f), 0, UB.TileDimensions.y - 1);
    int tile_max_y = clamp((int)floor(min(float(C.FilmDimensions.y) - 1.0f, max_sy_bb) / 16.0f), 0, UB.TileDimensions.y - 1);

    if (LocalID == 0) {
        SharedNumSortKeysCompacted = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    int coarse_allocation = (tile_max_x - tile_min_x + 1) * (tile_max_y - tile_min_y + 1);


    uint GroupWriteOffset = 0;
    InterlockedAdd(SharedNumSortKeysCompacted, (uint)coarse_allocation, GroupWriteOffset);
    GroupMemoryBarrierWithGroupSync();
    uint WriteOffset = 0;
    if (LocalID == 0) {
        InterlockedAdd(RWPrimitiveInstanceCount[0], SharedNumSortKeysCompacted, WriteOffset);
        SharedGlobalWriteOffset = WriteOffset;
    }
    GroupMemoryBarrierWithGroupSync();
    WriteOffset = SharedGlobalWriteOffset + GroupWriteOffset;

    uint NumSortKeysCompacted = 0;

    // 5. For each candidate tile, perform precise intersection test and generate instance data
    for (int ty = tile_min_y; ty <= tile_max_y; ++ty)
    {
        for (int tx = tile_min_x; tx <= tile_max_x; ++tx)
        {
            uint tile_id = (uint)tx + (uint)ty * UB.TileDimensions.x;
            if (tile_id >= (1u << 13)) continue;

            uint SortKey = PackSortKey(tile_id, QuantizedDepth);
            // Write directly, bypass the cache for compaction.
            uint WriteIndex = WriteOffset + NumSortKeysCompacted;
            if (WriteIndex < UB.MaxNumPrimitiveInstances) {
                RWPrimitiveInstanceListKeyBuffer[WriteIndex] = SortKey;
                RWPrimitiveInstanceListBuffer[WriteIndex] = RenderablePrimitiveIndex;
                NumSortKeysCompacted++;
            }
        }
    }
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void CollectTileInstanceOffsets(
    uint DispatchID: SV_DispatchThreadID
) {
    if (DispatchID >= PrimitiveInstanceCount[0]) {
        return; // No active instances to process
    }

    // Each thread processes one active primitive sort key
    uint sort_key = PrimitiveInstanceListKeySortedBuffer[DispatchID];
    uint prev_sort_key = 0;
    if (DispatchID > 0) prev_sort_key = PrimitiveInstanceListKeySortedBuffer[DispatchID - 1];
    else prev_sort_key = 0xffffffff; // Use a sentinel value for the first element
    uint current_tile = 0, prev_tile = 0, current_quant_depth = 0, prev_quant_depth = 0;
    UnpackSortKey(sort_key, current_tile, current_quant_depth);
    UnpackSortKey(prev_sort_key, prev_tile, prev_quant_depth);
    if (current_tile != prev_tile) {
        RWTileInstanceOffsetBuffer[current_tile] = DispatchID;
    }
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void CountTileInstances(
    uint DispatchID: SV_DispatchThreadID
) {
    if (DispatchID >= UB.NumTiles) {
        return;
    }
    uint TileIndex = DispatchID;
    uint Offset = TileInstanceOffsetBuffer[TileIndex];
    uint InstanceCount = PrimitiveInstanceCount[0];
    uint TileInstanceCount = 0;
    while(Offset + TileInstanceCount < InstanceCount) {
        uint SortKey = PrimitiveInstanceListKeySortedBuffer[Offset + TileInstanceCount];
        uint UnpackedTileIndex, QuantizedDepth;
        UnpackSortKey(SortKey, UnpackedTileIndex, QuantizedDepth);
        if(UnpackedTileIndex != TileIndex) {
            break; // Reached the end of this tile's instances
        }
        TileInstanceCount ++;
    }
    RWTileInstanceCountBuffer[TileIndex] = TileInstanceCount;
}

float TempFn(float l1, float r1, float s1, float l2, float r2, float s2, float x) {
    return (max(x - l1, 0) - max(x - r1, 0)) * s1 +
           (max(x - l2, 0) - max(x - r2, 0)) * s2;
}

RayVolumeDistribution UpdateRayVolumeDistribution(
    RayVolumeDistribution distr,
    RayVolumePrimitiveIntersection intersection,
    inout float Cdf,
    inout float attenuation
) {
    // TODO
    Cdf = 1.f;
    attenuation = 1.f;

    float l = distr.Density.l;
    float r = distr.Density.r;
    // sample nums per intersection
    int num_integrate_samples = 4;

    float half_period = r - l;
    float half_period_inv = 1.f / half_period;
    float omega = PI / half_period;
    // 计算该段体积在傅里叶级数中对应的长度
    float delta = (intersection.r - intersection.l) / float(num_integrate_samples);
    // 给每项傅里叶级数累加
    distr.Density.fourier_a[0] += 0.5f * half_period_inv * intersection.Density * (intersection.r - intersection.l);
    distr.WeightedColor.fourier_a[0] += 0.5f * half_period_inv * intersection.Density * intersection.Color * (intersection.r - intersection.l);
    for(int i = 1; i <= distr.Density.fourier_order; i++) {
        float fourier_density_a;
        float fourier_density_b;
        float3 weighted_fourier_color_a;
        float3 weighted_fourier_color_b;
        float n_omega = i * omega;
        for(int k = 0; k < num_integrate_samples; k++) {
            float x = delta * (k + 0.5f) + intersection.l - l;
            float n_omega_x = n_omega * x;
            fourier_density_a = half_period_inv * intersection.Density * cos(n_omega_x) * delta;
            fourier_density_b = half_period_inv * intersection.Density * sin(n_omega_x) * delta;
            distr.Density.fourier_a[i] += fourier_density_a;
            distr.Density.fourier_b[i] += fourier_density_b;
            weighted_fourier_color_a = half_period_inv * intersection.Density * intersection.Color * cos(n_omega_x) * delta;
            weighted_fourier_color_b = half_period_inv * intersection.Density * intersection.Color * sin(n_omega_x) * delta;
            distr.WeightedColor.fourier_a[i] += weighted_fourier_color_a;
            distr.WeightedColor.fourier_b[i] += weighted_fourier_color_b;
        }
    }
    return distr;
}

float ComputeTransmittance (float Depth, float Density) {
    return exp(-Density * Depth);
}

RayVolumeDistribution RenderRay(
    float3 RayOrigin,
    float3 RayDirection,
    uint TileInstanceOffset,
    uint NumTilePrimitiveInstances,
    float MaxLinearDepth,
    inout Random rng,
    inout float TotalTransmittance,
    inout float  SampleTransmittance,
    inout float3 SampleColor,
    inout float SampleDepth,
    inout float SamplePdf,
    inout float Cdf,
    inout float Attenuation
) {
    RayVolumeDistribution Result;
    Result.Density.l = 1e9f;
    Result.Density.r = 0.f;
    Result.WeightedColor.l = 1e9f;
    Result.WeightedColor.r = 0.f;
    // TODO:在ImGui中添加对傅里叶级数阶数的调控
    Result.Density.fourier_order = 3;
    Result.WeightedColor.fourier_order = 3;
    for (uint i = 0; i <= Result.Density.fourier_order; i++) {
        Result.Density.fourier_a[i] = 0.f;
        Result.Density.fourier_b[i] = 0.f;
        Result.WeightedColor.fourier_a[i] = float3(0.f, 0.f, 0.f);
        Result.WeightedColor.fourier_b[i] = float3(0.f, 0.f, 0.f);
    }
    Cdf = 1.f;
    SampleDepth = 1e9f;
    SampleColor = float3(0.f, 0.f, 0.f);
    TotalTransmittance = 1.f;

    // Restore all the Intersection information
    uint IntersectionCount = 0;
    RayVolumePrimitiveIntersection Intersections[128];

    // First loop:Get l & r and restore intersections
    for (uint i = 0; i < NumTilePrimitiveInstances; i++) {
        uint RenderablePrimitiveIndex = PrimitiveInstanceListSortedBuffer[TileInstanceOffset + i];
        uint PrimitiveIndex, RenderableIndex;
        UnpackRenderablePrimitiveIndex(RenderablePrimitiveIndex, RenderableIndex, PrimitiveIndex);
        VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveIndex);

        float3x4 ToObjectTransform = RenderableInverseTransformBuffer[RenderableIndex];

        // Calculate intersection with the primitive
        float2 lr; float Dist;
        bool bIntersected = RayIntersect(
            RayOrigin, RayDirection, Primitive, ToObjectTransform,
            lr, Dist
        );

        // Control intersection range to 0~MaxLinearDepth
        lr.x = max(lr.x, 0.f);
        lr.y = min(lr.y, MaxLinearDepth);
        // If intersection is valid, update l & r and restore it
        if(bIntersected && lr.y > lr.x) {
            Result.Density.l = min(Result.Density.l, lr.x);
            Result.Density.r = max(Result.Density.r, lr.y);
            Result.WeightedColor.l = min(Result.WeightedColor.l, lr.x);
            Result.WeightedColor.r = max(Result.WeightedColor.r, lr.y);

            Intersections[IntersectionCount].l = lr.x;
            Intersections[IntersectionCount].r = lr.y;
            //Intersections[IntersectionCount].Density = Primitive.Opacity;
            Intersections[IntersectionCount].Density = Primitive.Opacity * VolumePrimitiveRayDecay(Dist);
            Intersections[IntersectionCount].Color = Primitive.Color;
            IntersectionCount++;
        }
    }

    // Second loop:Calculate Fourier Density and Color
    for(uint i = 0; i < IntersectionCount; i++) {
        Result = UpdateRayVolumeDistribution(Result, Intersections[i], Cdf, Attenuation);
    }

    // 采样自由程
    float u = rng.rand();
    SampleDepth = SampleRayVolumeDistribution(Result, u);
    if(SampleDepth < Result.Density.r) {
        SampleColor = GetRayVolumeDistributionColor(Result, SampleDepth);
        SamplePdf = GetRayVolumeDistributionDensity(Result, SampleDepth);
        SampleTransmittance = exp(-IntegrateRayVolumeDistributionDensity(Result, SampleDepth));
    }
    else {
        SampleColor = float3(0.f, 0.f, 0.f);
        SamplePdf = 0.f;
        SampleTransmittance = exp(-IntegrateRayVolumeDistributionDensity(Result, Result.Density.r));
    }
    TotalTransmittance = exp(-IntegrateRayVolumeDistributionDensity(Result, Result.Density.r));
    return Result;
}

#define TILE_SIZE 16

Texture2D<float> G_Depth;

[[vk::image_format("r8ui")]]
RWTexture2D<uint> RWFlags;

// Dispatch 1 group per tile, each group processes a 16x16 tile of pixels
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void DrawVolumePrimitives (
    uint2 GroupID: SV_GroupID,
    uint2 LocalID: SV_GroupThreadID
) {
    uint TileIndex = GroupID.x + GroupID.y * UB.TileDimensions.x;
    uint TileInstanceOffset = TileInstanceOffsetBuffer[TileIndex];
    uint NumTilePrimitiveInstances = TileInstanceCountBuffer[TileIndex];
    {
        CameraParameters C = GetActiveCamera();
        uint2 PixelOffsetInTile = LocalID;
        uint2 PixelIndex = GroupID * TILE_SIZE + PixelOffsetInTile;
        if (all(PixelIndex < C.FilmDimensions)) {
            float3 RayOrigin = C.Position;
            float2 UV = ScreenCoordsToUV(C, PixelIndex);
            float3 RayDirection = NDC2ToCameraDirectionUnnormalized(C, UVToNDC2(UV));
            float ReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, UV, 0);
            float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
            float Cdf = 1.f, Attenuation = 1.f;
            float3 SampleColor = 0;
            float SampleTransmittance = 0;
            float SampleDepth = 0, SamplePdf = 0;
            float TotalTransmittance = 1.f;
            Random rng = MakeRandom(PixelIndex.x + PixelIndex.y * C.FilmDimensions.x, 17491741 + UB.FrameIndex);
            RayVolumeDistribution Rendered = RenderRay(
                RayOrigin, RayDirection, TileInstanceOffset, NumTilePrimitiveInstances,
                LinearDepth,
                rng,
                TotalTransmittance, SampleTransmittance, SampleColor, SampleDepth, SamplePdf,
                 Cdf, Attenuation);
            for(uint i = 0; i <= Rendered.Density.fourier_order; i++) {
                RWVolumeDensity[uint3(PixelIndex, i << 1)] = Rendered.Density.fourier_a[i];
                RWVolumeDensity[uint3(PixelIndex, (i << 1) + 1)] = Rendered.Density.fourier_b[i];
            }
            for(uint i = 0; i <= Rendered.WeightedColor.fourier_order; i++) {
                RWVolumeColor[uint3(PixelIndex, i << 1)] = float4(Rendered.WeightedColor.fourier_a[i], 1);
                RWVolumeColor[uint3(PixelIndex, (i << 1) + 1)] = float4(Rendered.WeightedColor.fourier_b[i], 1);
            }
            RWVolumeMinMax[PixelIndex] = float2(Rendered.Density.l, Rendered.Density.r);
            RWVolumeCdfAttenuation[PixelIndex] = float2(Cdf, Attenuation);
            RWVolumeSampleColorAndLinearDepth[PixelIndex] = float4(SampleColor, SampleDepth);
            RWVolumeSampleTransmittanceAndPdf[PixelIndex] = float2(SampleTransmittance, SamplePdf);
            RWTransmittance[PixelIndex] = TotalTransmittance;
            // Mark the pixel as invalid for SSRT if it overlaps with a volume
            uint OldFlags = RWFlags[PixelIndex];
            int Density_valid = 0;
            for(uint i = 0; i <= Rendered.Density.fourier_order; i++) {
                if(Rendered.Density.fourier_a[i] > 0.f) {
                    Density_valid = 1;
                    break;
                }
            }
            if(Density_valid == 1) OldFlags |= FLAG_BITS_TEXTURE_INVALID_FOR_SSRT;
            RWFlags[PixelIndex] = OldFlags;
        }
    }
}
