#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Packing.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedView.hlsl"
#include "shared/SharedVolumePrimitives.hlsl"
#include "headers/Transform.hlsl"

#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE 128
#endif

struct RenderVolumePrimitivesUB {
    uint2 TileDimensions;
    float ExpandFactor;
    uint NumTiles;
    uint MaxNumPrimitiveInstances;
    uint3 Padding;
};

struct CollectVolumePrimitivesUB {
    uint RenderableIndex;
    uint InstancePrimitiveOffset;
    uint InstanceNumPrimitives;
    uint Padding;
};
ConstantBuffer<RenderVolumePrimitivesUB> UB;
ConstantBuffer<CollectVolumePrimitivesUB> UB_Collect;

RWStructuredBuffer<uint> RWActivePrimitiveCount;
RWStructuredBuffer<uint> RWTileInstanceOffsets;
RWStructuredBuffer<uint> RWPrimitiveInstanceCount;

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void VolumePrimitivesClearCounters(uint DispatchID: SV_DispatchThreadID) {
    if (DispatchID == 0) {
        RWActivePrimitiveCount[0] = 0;
        RWPrimitiveInstanceCount[0] = 0;
    }
    if(DispatchID < UB.NumTiles) {
        RWTileInstanceOffsets[DispatchID] = 0;
    }
}

StructuredBuffer<PackedVolumePrimitive> PrimitiveData;
RWStructuredBuffer<uint> RWActivePrimitiveList;
StructuredBuffer<float3x4> RenderableTransforms;

VolumePrimitive LoadVolumePrimitive(StructuredBuffer<PackedVolumePrimitive> PrimitiveData, uint Index) {
    PackedVolumePrimitive PackedPrimitive = PrimitiveData[Index];
    VolumePrimitive Primitive;
    Primitive.Position = PackedPrimitive.Position;
    float4 Rotation = normalize(UnpackSnorm4x8(PackedPrimitive.PackedRotation));
    Primitive.Scales = PackedPrimitive.Scales;
    float4 ColorOpacity = UnpackSnorm4x8(PackedPrimitive.PackedColorOpacity);
    Primitive.Color = ColorOpacity.rgb;
    Primitive.Opacity = ColorOpacity.a;
    return Primitive;
}

uint PackRenderablePrimitiveIndex (uint InstanceIndex, uint PrimitiveIndex) {
    // 12 bits for instance index, 20 bits for primitive index
    return (InstanceIndex << 20) | PrimitiveIndex;
}

void UnpackRenderablePrimitiveIndex(uint PackedIndex, out uint InstanceIndex, out uint PrimitiveIndex) {
    InstanceIndex = PackedIndex >> 20;
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
    VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveData, PrimitiveIndex);
    float3 center = Primitive.Position;
    float3 scales = Primitive.Scales;
    float3x4 ObjectToWorld = RenderableTransforms[UB_Collect.RenderableIndex];
    float4 WorldCenterW = float4(mul(ObjectToWorld, float4(center, 1)), 1);
    float4 NDCCenterW = mul(GetActiveCamera().WorldToNDC, WorldCenterW);
    float3 NDCCenter = NDCCenterW.xyz / NDCCenterW.w;
    // Check if the center is within the view frustum
    uint bIsActive = 1;
    if (NDCCenterW.w < 0 || any(NDCCenter.xy < -1.3) || any(NDCCenter.xy > 1.3) || NDCCenter.z < 0.01 || NDCCenter.z > 1) {
        // Outside of the view frustum, skip this primitive
        bIsActive = 0;
    }
    float volume = scales.x * scales.y * scales.z;
    if (volume < 1e-6f) {
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
        RWActivePrimitiveList[ListOffset + GroupActivePrimitiveListOffset] = PackRenderablePrimitiveIndex(UB_Collect.RenderableIndex, PrimitiveIndex);
    }
}

uint PackSortKey(uint tile_id, uint quantized_depth) {
    uint sort_key = (tile_id << 19) | quantized_depth;
    return sort_key;
}

void UnpackSortKey(uint sort_key, out uint tile_id, out uint quantized_depth) {
    tile_id = sort_key >> 19;
    quantized_depth = sort_key & ((1u << 19) - 1u);
}

float3 GetCovariance2DMatrix(float3x3 view_matrix, float3x4 ToWorldTransform, VolumePrimitive Primitive, out float4 ClipPosW, out uint quantized_depth) {
    // 1. Transform center to view and clip space, calculate depth
    float4 WorldPosW = float4(TransformPoint(ToWorldTransform, Primitive.Position), 1.0f);
    CameraParameters C = GetActiveCamera();
    float4 ViewPosW = mul(C.WorldToView, WorldPosW);
    ClipPosW = mul(C.WorldToNDC, WorldPosW);

    // Depth is guaranteed to be in [0,1] for centers of active primitives, as per user spec (0.1 to 1).
    // And ClipPosW.w > 0 is also implied.
    float depth01 = ClipPosW.z / ClipPosW.w;
    quantized_depth = (uint)(clamp(depth01, 0.0f, 1.0f) * ((1u << 19) - 1u)); // Clamp for safety

    // 2. EWA-like projection: Compute 2D screen-space covariance matrix (Sigma_2D)
    float tan_fov_y_half = C.TanFoVY_2;
    float f_y_screen = (0.5f * C.FilmDimensions.y) / tan_fov_y_half;
    float tan_fov_x_half = tan_fov_y_half * C.FilmAspectRatioAndInvAspectRatio.x;
    float f_x_screen = (0.5f * C.FilmDimensions.x) / tan_fov_x_half;

    float xc = ViewPosW.x;
    float yc = ViewPosW.y;
    float zc = ViewPosW.z;

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

StructuredBuffer<uint> ActivePrimitiveCount;
StructuredBuffer<uint> ActivePrimitiveList;
RWStructuredBuffer<uint> RWPrimitiveInstanceListKey;
RWStructuredBuffer<uint> RWPrimitiveInstanceListPrimitiveIndex;

groupshared uint shared_num_sort_keys_compacted;
groupshared uint shared_global_write_offset;

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
    if (DispatchID >= ActivePrimitiveCount[0])
    {
        return;
    }

    uint RenderableIndex, PrimitiveIndex;
    UnpackRenderablePrimitiveIndex(ActivePrimitiveList[DispatchID], RenderableIndex, PrimitiveIndex);
    VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveData, PrimitiveIndex);

    CameraParameters C = GetActiveCamera();
    float4 clip_pos_h; uint quantized_depth;

    float3 cov2d = GetCovariance2DMatrix(To3x3(C.WorldToView), RenderableTransforms[RenderableIndex], Primitive, clip_pos_h, quantized_depth);

    // 3. Determine screen space bounding box of the projected 2D ellipse
    // For uniform distributions, K_SQ_ELLIPSE_BOUNDARY == 1
    float radius_x_approx = sqrt(K_SQ_ELLIPSE_BOUNDARY * max(0.0f, cov2d.x));
    float radius_y_approx = sqrt(K_SQ_ELLIPSE_BOUNDARY * max(0.0f, cov2d.z));

    float ndc_center_x = clip_pos_h.x / clip_pos_h.w;
    float ndc_center_y = clip_pos_h.y / clip_pos_h.w;

    float screen_center_x = (ndc_center_x * 0.5f + 0.5f) * float(C.FilmDimensions.x);
    float screen_center_y = ((1.0f - ndc_center_y) * 0.5f) * float(C.FilmDimensions.y);
    float2 ellipse_screen_center = float2(screen_center_x, screen_center_y);

    float min_sx_bb = screen_center_x - radius_x_approx;
    float max_sx_bb = screen_center_x + radius_x_approx;
    float min_sy_bb = screen_center_y - radius_y_approx;
    float max_sy_bb = screen_center_y + radius_y_approx;

    // 4. Determine overlapping tiles based on the bounding box

    int tile_min_x = clamp((int)floor(max(0.0f, min_sx_bb) / 16.0f), 0, UB.TileDimensions.x - 1);
    int tile_max_x = clamp((int)floor(min(float(C.FilmDimensions.x) - 1.0f, max_sx_bb) / 16.0f), 0, UB.TileDimensions.x - 1);
    int tile_min_y = clamp((int)floor(max(0.0f, min_sy_bb) / 16.0f), 0, UB.TileDimensions.y - 1);
    int tile_max_y = clamp((int)floor(min(float(C.FilmDimensions.y) - 1.0f, max_sy_bb) / 16.0f), 0, UB.TileDimensions.y - 1);

    if (LocalID == 0) {
        shared_num_sort_keys_compacted = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    int coarse_allocation = (tile_max_x - tile_min_x + 1) * (tile_max_y - tile_min_y + 1);


    uint group_write_offset = 0;
    InterlockedAdd(shared_num_sort_keys_compacted, (uint)coarse_allocation, group_write_offset);
    GroupMemoryBarrierWithGroupSync();
    uint write_offset = 0;
    if (LocalID == 0) {
        InterlockedAdd(RWPrimitiveInstanceCount[0], shared_num_sort_keys_compacted, write_offset);
        shared_global_write_offset = write_offset;
    }
    GroupMemoryBarrierWithGroupSync();
    write_offset = shared_global_write_offset + group_write_offset;

    uint num_sort_keys_compacted = 0;

    // 5. For each candidate tile, perform precise intersection test and generate instance data
    for (int ty = tile_min_y; ty <= tile_max_y; ++ty)
    {
        for (int tx = tile_min_x; tx <= tile_max_x; ++tx)
        {

            float4 tile_rect_pixels = float4(
                float(tx * 16),       // min_x
                float(ty * 16),       // min_y
                float((tx + 1) * 16), // max_x
                float((ty + 1) * 16)  // max_y
            );

            uint tile_id = (uint)tx + (uint)ty * UB.TileDimensions.x;
            if (tile_id >= (1u << 13)) continue;

            uint sort_key = PackSortKey(tile_id, quantized_depth);
            // Write directly, bypass the cache for compaction.
            uint write_index = write_offset + num_sort_keys_compacted;
            if (write_index < UB.MaxNumPrimitiveInstances) {
                RWPrimitiveInstanceListKey[write_index] = sort_key;
                RWPrimitiveInstanceListPrimitiveIndex[write_index] = PrimitiveIndex;
                num_sort_keys_compacted++;
            }
        }
    }
}

StructuredBuffer<uint> PrimitiveInstanceCount;
StructuredBuffer<uint> PrimitiveInstanceListKeySorted;

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void CollectTileInstanceOffsets(
    uint DispatchID: SV_DispatchThreadID
) {
    if (DispatchID >= PrimitiveInstanceCount[0]) {
        return; // No active instances to process
    }

    // Each thread processes one active primitive sort key
    uint sort_key = PrimitiveInstanceListKeySorted[DispatchID];
    uint prev_sort_key = 0;
    if (DispatchID > 0) prev_sort_key = PrimitiveInstanceListKeySorted[DispatchID - 1];
    else prev_sort_key = 0xffffffff; // Use a sentinel value for the first element
    uint current_tile = 0, prev_tile = 0, current_quant_depth = 0, prev_quant_depth = 0;
    UnpackSortKey(sort_key, current_tile, current_quant_depth);
    UnpackSortKey(prev_sort_key, prev_tile, prev_quant_depth);
    if (current_tile != prev_tile) {
        RWTileInstanceOffsets[current_tile] = DispatchID;
    }
}

struct RayVolumeDistribution {
    float l, r;
    float density;
    float3 color;
    float cdf;
};

float TempFn(float l1, float r1, float s1, float l2, float r2, float s2, float x) {
    return (max(x - l1, 0) - max(x - r1, 0)) * s1 +
           (max(x - l2, 0) - max(x - r2, 0)) * s2;
}

RayVolumeDistribution UpdateRayVolumeDistribution(RayVolumeDistribution old_distr, RayVolumeDistribution new_distr)
{
    float old_l = old_distr.l;
    float old_r = old_distr.r;
    float new_l = new_distr.l;
    float new_r = new_distr.r;
    float x_val[4];
    // Sort the boundaries
    x_val[0] = min(old_l, new_l);
    x_val[1] = max(old_l, new_l);
    x_val[2] = min(old_r, new_r);
    x_val[3] = max(old_r, new_r);
    if (x_val[1] > x_val[2]) {
        float tmp = x_val[2];
        x_val[2] = x_val[1];
        x_val[1] = tmp;
    }
    float y_val[4];
    y_val[0] = TempFn(old_l, old_r, old_distr.density, new_l, new_r, new_distr.density, x_val[0]);
    y_val[1] = TempFn(old_l, old_r, old_distr.density, new_l, new_r, new_distr.density, x_val[1]);
    y_val[2] = TempFn(old_l, old_r, old_distr.density, new_l, new_r, new_distr.density, x_val[2]);
    y_val[3] = TempFn(old_l, old_r, old_distr.density, new_l, new_r, new_distr.density, x_val[3]);
    const float TARGET = 1.f;
    float x_sol = x_val[3];
    for (int segment = 0; segment < 3; segment++) {
        float x1 = x_val[segment];
        float x2 = x_val[segment + 1];
        float y1 = y_val[segment];
        float y2 = y_val[segment + 1];
        if (y1 <= TARGET && TARGET <= y2) {
            float seg_length = max(x2 - x1, 1e-5);
            float slope = (y2 - y1) / seg_length;
            if (abs(slope) < 1e-5f) {
                x_sol = x1;
            } else {
                float t = (TARGET - y1) / slope;
                x_sol = x1 + t;
            }
            // Find the first intersection
            break;
        }
    }
    float old_r_1 = min(old_r, x_sol);
    float new_r_1 = min(new_r, x_sol);
    float old_int_col = max(old_r_1 - old_l, 0) * old_distr.density;
    float new_int_col = max(new_r_1 - new_l, 0) * new_distr.density;
    float total_int_col = max(1e-5, old_int_col + new_int_col);
    float old_int = (old_r - old_l) * old_distr.density;
    float new_int = (new_r - new_l) * new_distr.density;
    float total_int = max(1e-5, old_int + new_int);

    RayVolumeDistribution result;
    // Strategy: Preserve boundaries
    result.l = min(old_l, new_l);
    result.r = max(old_r, new_r); 
    result.density = total_int / (result.r - result.l);
    // Blend color with special rules.
    result.color = (old_distr.color * old_int_col + new_distr.color * new_int_col) / total_int_col;
    return result;
}

StructuredBuffer<uint> TileInstanceOffsets;
StructuredBuffer<uint> PrimitiveInstanceListPrimitiveIndexSorted;

#define TILE_SIZE 16

// Dispatch 1 group per tile, each group processes a 16x16 tile of pixels
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void DrawVolumePrimitives (
    uint2 GroupID: SV_GroupID,
    uint2 LocalID: SV_GroupThreadID
) {
    uint tile_index = GroupID.x + GroupID.y * UB.TileDimensions.x;
    uint instance_primitive_list_offset = TileInstanceOffsets[tile_index];
    if(!IsValid(instance_primitive_list_offset)) {
        // No primitives in this tile, skip rendering
        return;
    }
    {
        CameraParameters C = GetActiveCamera();
        uint2 pixel_offset_in_tile = GroupID * TILE_SIZE + LocalID;
        uint2 pixel_index = GroupID * 16 + pixel_offset_in_tile;
        //if (all(pixel_index < C.FilmDimensions)) {
        //    float3 ray_origin = C.Position;
        //    float3 ray_direction = GetRayDirection(pixel_index, ub);

            // Per pixel weight (d final_loss / d per_pixel_loss)
        //    float dl_dpixelloss = 1.0f / (float)(C.FilmDimensions.x * C.FilmDimensions.y);
            // Forward pass
        //    float4 rendered = RenderRay_Forward(ray_origin, ray_direction,
        //                                        primitive_data_ptr, forward_state_cache_buf, pixel_cache_offset,
        //                                        active_instance_primitive_list_sorted_buf, instance_primitive_list_offset,
        //                                        num_primitives);
        //    output_texture[pixel_index] = float4(rendered_pair.p.rgb * rendered_pair.p.w, 1.f);
        //}
    }
}