#include "headers/Camera.hlsl"
#include "headers/VolumePrimitive.hlsl"
#include "headers/Transform.hlsl"

#define COMPUTE_DEFAULT_GROUP_SIZE 64

StructuredBuffer<PackedVolumePrimitive> PrimitiveDataBuffer;

RWStructuredBuffer<uint> RWActivePrimitiveCount;
RWStructuredBuffer<uint> RWActiveInstanceCount;
StructuredBuffer<uint> ActivePrimitiveCount;
StructuredBuffer<uint> ActiveInstanceCount;
RWStructuredBuffer<uint> RWActivePrimitiveListBuffer;
RWStructuredBuffer<uint> RWActiveInstanceSortingKeyBuffer;
RWStructuredBuffer<uint> RWActiveInstancePrimitiveListBuffer;
StructuredBuffer<uint> ActiveInstanceSortingKeySortedBuffer;
StructuredBuffer<uint> ActiveInstancePrimitiveListSortedBuffer;
StructuredBuffer<uint> TileInstancePrimitiveListOffsetBuffer;
StructuredBuffer<uint> TileInstancePrimitiveListCountsBuffer;

Texture2D<float4> RWOutputTexture;

struct UniformBuffer {
    uint PrimitiveCount;
    uint MaxNumInstances;
    uint2 TileDimensions;
};
ConstantBuffer<UniformBuffer> UB;


[numthreads(1, 1, 1)]
void ClearCounters() {
    RWActiveInstanceCount[0] = 0;
    RWActivePrimitiveCount[0] = 0;
}


groupshared uint SharedGroupActivePrimitiveCount;
groupshared uint SharedListOffset;
[shader("compute")]
[numthreads(COMPUTE_DEFAULT_GROUP_SIZE, 1, 1)]
void FilterActivePrimitives(
    uint local_index: SV_GroupThreadID,
    uint global_index: SV_DispatchThreadID,
) {
    // Simply cull primitives whose centers are too far outside of the view frustum.
    if (global_index >= UB.PrimitiveCount) return;
    // Corrected DiffBufferPointer: offset should be 0 if primitive_data_buf is the base buffer.
    // LoadSphereDistribution uses 'global_index' as the primitive index.
    VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveDataBuffer, global_index);
    float3 Center = Primitive.Position;
    float3 Scales = Primitive.Scales;
    float4 NDCCenterW = mul(GetActiveCamera().WorldToNDC, float4(Center, 1.0f));
    float3 NDCCenter = NDCCenterW.xyz / NDCCenterW.w;
    // Check if the Center is within the view frustum
    uint bIsActive = 1;
    if (NDCCenterW.w < 0 || any(NDCCenter.xy < -1.3) || any(NDCCenter.xy > 1.3) || NDCCenter.z < 0.01 || NDCCenter.z > 1) {
        // Outside of the view frustum, skip this primitive
        bIsActive = 0;
    }
    float Volume = Scales.x * Scales.y * Scales.z;
    if (Volume < 1e-6f) {
        // Skip degenerate primitives (too small)
        bIsActive = 0;
    }
    if (Primitive.Opacity <= 0.001) {
        // Skip transparent primitives
        bIsActive = 0;
    }
    if (local_index == 0) {
        // Reset the group active primitive count
        SharedGroupActivePrimitiveCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    uint GroupActivePrimitiveListIndex = 0, ListOffset = 0;
    InterlockedAdd(SharedGroupActivePrimitiveCount, bIsActive, GroupActivePrimitiveListIndex);
    GroupMemoryBarrierWithGroupSync();
    if (local_index == 0) {
        uint GroupActiveCount = SharedGroupActivePrimitiveCount;
        InterlockedAdd(RWActivePrimitiveCount[0], GroupActiveCount, ListOffset);
        SharedListOffset = ListOffset;
    }
    GroupMemoryBarrierWithGroupSync();
    ListOffset = SharedListOffset;
    if (bIsActive != 0) {
        // Store the primitive index in the active primitive list
        RWActivePrimitiveListBuffer[ListOffset + GroupActivePrimitiveListIndex] = global_index;
    }
}

// Constant for ellipse boundary determination. For a uniform ellipsoid whose
// covariance matrix Sigma_screen_2D defines its boundary via (P-C)^T Sigma_inv (P-C) = 1,
// this should be 1.0. Use a 0.1f padding for large ellipses that makes EWA approximation fail.
static const float K_SQ_ELLIPSE_BOUNDARY = 1.f + 0.1f;

// Helper function to check if a point is inside a 2D ellipse
// Ellipse equation: s11*(x-cx)^2 - 2*s01*(x-cx)*(y-cy) + s00*(y-cy)^2 <= detS * K_sq
// where S = [[s00, s01], [s01, s11]] is the covariance matrix.
bool IsPointInEllipse(float2 p, float2 Center, float s00, float s01, float s11, float detS_times_Ksq) {
    float dx = p.x - Center.x;
    float dy = p.y - Center.y;
    return (s11 * dx * dx - 2.0f * s01 * dx * dy + s00 * dy * dy) <= detS_times_Ksq;
}

// Helper function to check for intersection between a 2D ellipse and an axis-aligned tile rectangle
// This version transforms the tile into the ellipse's local space where the ellipse is a unit circle.
bool EllipseTileIntersection(
    float2 ellipse_Center_s,         // screen space Center of the ellipse
    float s00, float s01, float s11, // elements of Sigma_screen_2D (covariance matrix)
    float4 tile_rect_s               // (min_x, min_y, max_x, max_y) for the tile in screen pixels
) {
    // TODO this function seems to be producing false neagtives.
    float detS = s00 * s11 - s01 * s01;
    if (detS <= 1e-9f) { // Degenerate ellipse (line or point)
        // Fallback: check if ellipse Center is within tile bounds (treat as point)
        return ellipse_Center_s.x >= tile_rect_s.x && ellipse_Center_s.x <= tile_rect_s.z &&
               ellipse_Center_s.y >= tile_rect_s.y && ellipse_Center_s.y <= tile_rect_s.w;
    }

    // Eigen decomposition of Sigma_screen_2D
    // Sigma = U * L_diag * U^T
    // L_diag = [[L1, 0], [0, L2]]
    float trace = s00 + s11;
    // Discriminant: (s00-s11)^2 + 4*s01^2 = trace^2 - 4*detS
    float discriminant = trace * trace - 4.0f * detS;
    // Clamp discriminant to non-negative in case of numerical issues for near-degenerate ellipses
    float sqrt_discriminant = sqrt(max(0.0f, discriminant));

    float L1 = (trace + sqrt_discriminant) / 2.0f;
    float L2 = (trace - sqrt_discriminant) / 2.0f;

    // Transformation to local space (where ellipse is x^2/L1 + y^2/L2 = K_SQ_ELLIPSE_BOUNDARY)
    // and then to unit circle space (x_local^2 + y_local^2 = 1)
    // P_local = diag(1/sqrt(L1*K_SQ), 1/sqrt(L2*K_SQ)) * U^T * (P_screen - C_screen)
    // K_SQ_ELLIPSE_BOUNDARY is 1.0f here.
    float inv_sqrt_L1 = 1.0f / sqrt(max(1e-9f, L1)); // max to avoid div by zero
    float inv_sqrt_L2 = 1.0f / sqrt(max(1e-9f, L2));

    float2x2 U_T; // Transpose of eigenvector matrix U
    if (abs(s01) < 1e-6f) {                               // Diagonal or near-diagonal matrix
        U_T = float2x2(1, 0, 0, 1); // Eigenvectors are axis-aligned
        if (s00 < s11) {   // Ensure L1 corresponds to s00 if s00 is larger, or s11 if s11 is larger
            // If s00 is L1, s11 is L2, U is I.
            // If s00 is L2, s11 is L1, U is [[0,1],[1,0]], U_T is [[0,1],[1,0]]
            // The eigenvalue calculation already sorts L1 >= L2 if discriminant is positive.
            // If s01 is 0, L1 = max(s00,s11), L2 = min(s00,s11).
            // If s00 was originally smaller, and became L1, then axes swapped.
            // This case is fine, U=I.
        }
    } else {
        // Eigenvector for L1: (s01, L1 - s00)
        float2 e1 = normalize(float2(s01, L1 - s00));
        // Eigenvector for L2 is orthogonal to e1
        float2 e2 = float2(-e1.y, e1.x);
        U_T = float2x2(e1.x, e1.y, e2.x, e2.y); // U_T has eigenvectors as rows
    }

    float2x2 M_transform_diag_part = float2x2(inv_sqrt_L1, 0, 0, inv_sqrt_L2);
    float2x2 M_transform = mul(M_transform_diag_part, U_T);

    // Tile vertices in screen space
    float2 tc_s[4];
    tc_s[0] = float2(tile_rect_s.x, tile_rect_s.y); // top-left
    tc_s[1] = float2(tile_rect_s.z, tile_rect_s.y); // top-right
    tc_s[2] = float2(tile_rect_s.z, tile_rect_s.w); // bottom-right
    tc_s[3] = float2(tile_rect_s.x, tile_rect_s.w); // bottom-left

    // Transform tile vertices to ellipse local space
    float2 tc_l[4];
    for (int i = 0; i < 4; ++i) {
        tc_l[i] = mul(M_transform, tc_s[i] - ellipse_Center_s);
    }

    // --- Intersection tests for transformed quadrilateral tc_l with unit circle at origin ---

    // 1. Any transformed tile vertex inside unit circle?
    for (int i = 0; i < 4; ++i) {
        if (dot(tc_l[i], tc_l[i]) <= 1.0f) return true;
    }

    // 2. Unit circle Center (origin) inside transformed tile?
    //    Using cross products: (v_i - O) x (v_{i+1} - O) must all have same sign for O to be inside.
    //    Since O is (0,0), this is v_i x v_{i+1}.
    float sign_check = 0.0f;
    bool origin_inside = true;
    for (int i = 0; i < 4; ++i) {
        float2 p1 = tc_l[i];
        float2 p2 = tc_l[(i + 1) % 4];
        float cross_product_z = p1.x * p2.y - p1.y * p2.x;
        if (i == 0) {
            sign_check = cross_product_z > 0 ? 1.0f : (cross_product_z < 0 ? -1.0f : 0.0f);
        } else if (sign_check != 0.0f) { // If first edge was not degenerate
            float current_sign = cross_product_z > 0 ? 1.0f : (cross_product_z < 0 ? -1.0f : 0.0f);
            if (current_sign != 0.0f && current_sign != sign_check) {
                origin_inside = false;
                break;
            }
        } else { // First edge was degenerate, try to establish sign_check with current edge
            sign_check = cross_product_z > 0 ? 1.0f : (cross_product_z < 0 ? -1.0f : 0.0f);
        }
        // If all cross products are zero (collinear points forming a line segment passing through origin),
        // origin_inside might be true if origin is on the segment. This is covered by edge checks.
    }
    // If sign_check ended up being 0 (all points collinear and on a line through origin),
    // this test is inconclusive for "inside", rely on edge checks.
    if (origin_inside && sign_check != 0.0f) return true;

    // 3. Any edge of transformed tile intersects unit circle?
    //    (Assumes vertices are not inside, from test 1)
    for (int i = 0; i < 4; ++i) {
        float2 p1 = tc_l[i];
        float2 p2 = tc_l[(i + 1) % 4];
        float2 d = p2 - p1;
        float dr2 = dot(d, d);

        if (dr2 < 1e-9f) continue; // Degenerate edge (p1 ~ p2)

        // t = dot(-p1, d) / dr2
        // Closest point on line p1-p2 to origin is P = p1 + t*d
        float t = dot(-p1, d) / dr2;

        float2 closest_point_on_line;
        if (t < 0.0f) closest_point_on_line = p1;
        else if (t > 1.0f) closest_point_on_line = p2;
        else closest_point_on_line = p1 + t * d;

        // If closest point on segment to origin is within unit circle
        if (dot(closest_point_on_line, closest_point_on_line) <= 1.0f) return true;
    }

    return false;
}

uint PackSortKey(uint TileIndex, uint QuantizedDepth) {
    uint SortKey = (TileIndex << 19) | QuantizedDepth;
    return SortKey;
}

void UnpackSortKey(uint SortKey, out uint TileIndex, out uint QuantizedDepth) {
    TileIndex = SortKey >> 19;
    QuantizedDepth = SortKey & ((1u << 19) - 1u);
}

#define MAX_CACHED_SORT_KEYS (8192 - 256) // at most 48kb shared memory
groupshared uint shared_cached_sort_keys[MAX_CACHED_SORT_KEYS];
groupshared uint shared_num_sort_keys;
groupshared uint shared_num_sort_keys_compacted;
groupshared uint shared_global_write_offset;

[shader("compute")]
[numthreads(COMPUTE_DEFAULT_GROUP_SIZE, 1, 1)]
void PrecomputeActivePrimitives(
    uint global_index: SV_DispatchThreadID,
    uint local_index: SV_GroupThreadID
) {
    // This shader is used to precompute necessary data for the active primitives.
    // It projects active 3D primitives to 2D, determines tile overlap,
    // and generates sort keys (tile_id, depth) and other instance data.

    // Each thread processes one active primitive from the input list.
    // ActivePrimitiveCount[0] stores the number of active primitives from FilterActivePrimitives pass.
    if (global_index >= ActivePrimitiveCount[0])
    {
        return;
    }

    uint PrimitiveIndex = RWActivePrimitiveListBuffer[global_index];
    VolumePrimitive Primitive = LoadVolumePrimitive(PrimitiveDataBuffer, PrimitiveIndex);

    // 1. Transform Center to view and clip space, calculate depth
    float4 world_pos_h = float4(Primitive.Position, 1.0f);
    float3x3 View = GetActiveCamera().WorldToView;
    float4 view_pos_h = mul(View, world_pos_h);
    float4 clip_pos_h = mul(GetActiveCamera().WorldToNDC, world_pos_h);

    // Depth is guaranteed to be in [0,1] for Centers of active primitives, as per user spec (0.1 to 1).
    // And clip_pos_h.w > 0 is also implied.
    float depth01 = clip_pos_h.z / clip_pos_h.w;
    uint quantized_depth = (uint)(clamp(depth01, 0.0f, 1.0f) * ((1u << 19) - 1u)); // Clamp for safety

    // 2. EWA-like projection: Compute 2D screen-space covariance matrix (Sigma_2D)
    float tan_fov_y_half = tan(GetActiveCamera().FoVY / 2.0f);
    float f_y_screen = (0.5f * GetActiveCamera().FilmDimensions.y) / tan_fov_y_half;
    float tan_fov_x_half = tan_fov_y_half * GetActiveCamera().FilmAspectRatioAndInvAspectRatio.x;
    float f_x_screen = (0.5f * GetActiveCamera().FilmDimensions.x) / tan_fov_x_half;

    float xc = view_pos_h.x;
    float yc = view_pos_h.y;
    float zc = view_pos_h.z;

    // Assume that the positive z-axis is aligned with view direction (left-handed view-space)
    if (zc <= 0.001f) // Primitive too close or behind camera (extra safety, though FilterActivePrimitives should handle)
    {
        return;
    }

    float inv_zc = 1.0f / zc;
    float inv_zc_sq = inv_zc * inv_zc;

    float2x3 J;
    J[0] = float3(f_x_screen * inv_zc, 0.0f, -f_x_screen * xc * inv_zc_sq);
    J[1] = float3(0.0f, f_y_screen * inv_zc, -f_y_screen * yc * inv_zc_sq);

    float3x3 Rot_s = BuildRotationMatrix(Primitive.Rotation);
    float3x3 Scales_sq_diag = float3x3(0);
    Scales_sq_diag[0][0] = Primitive.Scales.x * Primitive.Scales.x;
    Scales_sq_diag[1][1] = Primitive.Scales.y * Primitive.Scales.y;
    Scales_sq_diag[2][2] = Primitive.Scales.z * Primitive.scales.z;

    float3x3 Sigma_world_3D = mul(mul(Rot_s, Scales_sq_diag), transpose(Rot_s));

    float3x3 V_rot = float3x3(View._m00, View._m01, View._m02,
                              View._m10, View._m11, View._m12,
                              View._m20, View._m21, View._m22);
    float3x3 Sigma_view_3D = mul(mul(V_rot, Sigma_world_3D), transpose(V_rot));

    float2x2 Sigma_screen_2D = mul(mul(J, Sigma_view_3D), transpose(J));

    // Covariance matrix elements: S = [[s00, s01], [s01, s11]]
    float s00 = Sigma_screen_2D[0][0]; // Corresponds to variance in x if axes aligned
    float s01 = Sigma_screen_2D[0][1]; // Corresponds to covariance xy
    float s11 = Sigma_screen_2D[1][1]; // Corresponds to variance in y if axes aligned

    // 3. Determine screen space bounding box of the projected 2D ellipse
    // For uniform distributions, K_SQ_ELLIPSE_BOUNDARY == 1
    float radius_x_approx = sqrt(K_SQ_ELLIPSE_BOUNDARY * max(0.0f, s00));
    float radius_y_approx = sqrt(K_SQ_ELLIPSE_BOUNDARY * max(0.0f, s11));

    float ndc_Center_x = clip_pos_h.x / clip_pos_h.w;
    float ndc_Center_y = clip_pos_h.y / clip_pos_h.w;

    float screen_Center_x = (ndc_Center_x * 0.5f + 0.5f) * float(GetActiveCamera().FilmDimensions.x);
    float screen_Center_y = ((1.0f - ndc_Center_y) * 0.5f) * float(GetActiveCamera().FilmDimensions.y);
    float2 ellipse_screen_Center = float2(screen_Center_x, screen_Center_y);

    float min_sx_bb = screen_Center_x - radius_x_approx;
    float max_sx_bb = screen_Center_x + radius_x_approx;
    float min_sy_bb = screen_Center_y - radius_y_approx;
    float max_sy_bb = screen_Center_y + radius_y_approx;

    // 4. Determine overlapping tiles based on the bounding box

    int tile_min_x = clamp((int)floor(max(0.0f, min_sx_bb) / 16.0f), 0, UB.TileDimensions.x - 1);
    int tile_max_x = clamp((int)floor(min(float(GetActiveCamera().FilmDimensions.x) - 1.0f, max_sx_bb) / 16.0f), 0, UB.TileDimensions.x - 1);
    int tile_min_y = clamp((int)floor(max(0.0f, min_sy_bb) / 16.0f), 0, UB.TileDimensions.y - 1);
    int tile_max_y = clamp((int)floor(min(float(GetActiveCamera().FilmDimensions.y) - 1.0f, max_sy_bb) / 16.0f), 0, UB.TileDimensions.y - 1);

    if (local_index == 0) {
        shared_num_sort_keys = 0;
        shared_num_sort_keys_compacted = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    int coarse_allocation = (tile_max_x - tile_min_x + 1) * (tile_max_y - tile_min_y + 1);

    uint group_cache_offset = 0;
    InterlockedAdd(shared_num_sort_keys, (uint)coarse_allocation, group_cache_offset);

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

            // Perform precise intersection test
            if (!EllipseTileIntersection(ellipse_screen_Center, s00, s01, s11, tile_rect_pixels)) {
                continue; // Skip this tile if no intersection
            }

            uint tile_id = (uint)tx + (uint)ty * UB.TileDimensions.x;
            if (tile_id >= (1u << 13)) continue;

            uint sort_key = PackSortKey(tile_id, quantized_depth);

            uint shared_write_index = group_cache_offset + num_sort_keys_compacted;
            if (shared_write_index < MAX_CACHED_SORT_KEYS) {
                shared_cached_sort_keys[shared_write_index] = sort_key;
                num_sort_keys_compacted++;
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    // 6. Write the compacted sort keys to the output buffer
    uint group_compacted_write_offset = 0;
    InterlockedAdd(shared_num_sort_keys_compacted, num_sort_keys_compacted, group_compacted_write_offset);
    GroupMemoryBarrierWithGroupSync();
    if (local_index == 0) {
        // Accumulate the total number of sort keys compacted in this group
        uint global_sort_keys_compacted = shared_num_sort_keys_compacted;
        uint global_write_offset = 0;
        InterlockedAdd(RWActiveInstanceCount[0], global_sort_keys_compacted, global_write_offset);
        // Write result to shared memory ActivePrimitiveCount other threads
        shared_global_write_offset = global_write_offset;
    }
    GroupMemoryBarrierWithGroupSync();
    uint global_offset = shared_global_write_offset;
    for (int i = 0; i < num_sort_keys_compacted; i++) {
        uint read_index = group_cache_offset + i;
        uint write_index = global_offset + group_compacted_write_offset + i;
        if (read_index < MAX_CACHED_SORT_KEYS && write_index < UB.MaxNumInstances) {
            RWActiveInstanceSortingKeyBuffer[write_index] = shared_cached_sort_keys[read_index];
            RWActiveInstancePrimitiveListBuffer[write_index] = PrimitiveIndex;
        }
    }
}

[shader("compute")]
[numthreads(COMPUTE_DEFAULT_GROUP_SIZE, 1)]
void FindTileBoundaries(
    uint global_index: SV_DispatchThreadID
) {
    if (global_index >= ActiveInstanceCount[0]) {
        return; // No active instances to ActivePrimitiveCount
    }

    // Each thread processes one active primitive sort key
    uint sort_key = ActiveInstanceSortingKeySortedBuffer[global_index];
    uint prev_sort_key = 0;
    if (global_index > 0) prev_sort_key = ActiveInstanceSortingKeySortedBuffer[global_index - 1];
    else prev_sort_key = 0xffffffff; // Use a sentinel value for the first element
    uint current_tile = 0, prev_tile = 0, current_quant_depth = 0, prev_quant_depth = 0;
    UnpackSortKey(sort_key, current_tile, current_quant_depth);
    UnpackSortKey(prev_sort_key, prev_tile, prev_quant_depth);
    if (current_tile != prev_tile) {
        TileInstancePrimitiveListOffsetBuffer[current_tile] = global_index;
    }
}

// One thread per tile
[shader("compute")]
[numthreads(COMPUTE_DEFAULT_GROUP_SIZE, 1)]
void CountTileInstances(
    uint global_index: SV_DispatchThreadID
) {
    if (global_index >= UB.TileDimensions.x * UB.TileDimensions.y) {
        return;
    }
    uint offset = TileInstancePrimitiveListOffsetBuffer[global_index];
    uint instance_count = ActiveInstanceCount[0];
    uint count = ActivePrimitiveCount[0];
    while (true) {
        uint sort_key = ActiveInstanceSortingKeySortedBuffer[offset + count];
        uint tile_id, quantized_depth;
        UnpackSortKey(sort_key, tile_id, quantized_depth);
        if (tile_id != global_index) {
            break; // Reached the end of this tile's instances
        }
        count++;
        if (offset + count >= instance_count) {
            break; // Prevent out-of-bounds access
        }
    }
    TileInstancePrimitiveListCountsBuffer[global_index] = count;
}

float4 RenderRay () {
    
}

// Dispatch 1 group per tile, each group processes a 16x16 tile of pixels
[shader("compute")]
// Use a minimum wave size (rather than 32x32) for larger shared memory per thread
// Thus we can have longer differentiable loops within RenderRay function.
[numthreads(16, 16, 1)]
void Render(
    uint2 group_index: SV_GroupID,
    uint2 local_index: SV_GroupThreadID
) {
    uint tile_index = group_index.x + group_index.y * UB.TileDimensions.x;
    uint instance_primitive_list_offset = TileInstancePrimitiveListOffsetBuffer[tile_index];
    uint NumPrimitivesInTile = TileInstancePrimitiveListCountsBuffer[tile_index];
    if (NumPrimitivesInTile == 0) {
        // No primitives in this tile, skip rendering
        return;
    }
    {
        uint2 pixel_offset_in_tile = local_index;
        uint2 pixel_index = group_index * 16 + pixel_offset_in_tile;
        if (all(pixel_index < GetActiveCamera().FilmDimensions)) {
            float3 ray_origin = GetActiveCamera().Position;
            float2 NDC2 = UVToNDC2((pixel_index + 0.5f) / float2(GetActiveCamera().FilmDimensions));
            float3 ray_direction = NDC2ToCameraDirection(GetActiveCamera(), NDC2);

            float4 rendered = RenderRay(ray_origin, ray_direction,
                                        ActiveInstancePrimitiveListSortedBuffer, instance_primitive_list_offset,
                                        NumPrimitivesInTile);
            RWOutputTexture[pixel_index] = rendered;
            
        }
    }
}