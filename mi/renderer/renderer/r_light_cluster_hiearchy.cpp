/*
 * Created: 2026/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <renderer/r_light_cluster_hiearchy.h>
#include <renderer/mi_texture.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

MI_NAMESPACE_BEGIN

namespace {

/*
 * Algorithm overview
 *
 * Goal:
 * - Build a mesh-light hierarchy that increases sampling granularity (cluster instead of per-triangle)
 *   to reduce light-grid overflow and proposal complexity for future world-space ReSTIR pipelines.
 *
 * Core idea:
 * 1) Start from emissive triangles as leaf clusters.
 * 2) Build topology adjacency (shared edge).
 * 3) Iteratively merge the best pair (greedy agglomerative clustering) using a multi-objective cost.
 * 4) if no topology-adjacent pair is available, for the first N smallest (low emissive power) active cluster, find the best merge regardless of topology and merge them as a fallback. (N is specified as a constant, e.g. 8 or 16, to balance between quality and build time)
 * 5) Store the merge process as a tree.
 * 6) Capture independent LOD snapshots when active cluster count reaches N, N/2, N/4 ... 1.
 *
 * Why this shape:
 * - Tree storage keeps total node count O(N), avoiding O(N log N) duplication in hierarchy storage.
 * - Snapshot levels still provide easy GPU upload and budgeted runtime selection.P
 */


CVar<int> CVar_LightClusterBuildFallbackSearchN("r.light_grid.cluster_build_fallback_search_n",
    "Number of active clusters to search for best merge when no topology-adjacent pair is available during cluster hierarchy building." 
    "Higher value may improve quality but increase build time.",
    8
);

constexpr glm::vec3 kLuminanceCoeff = glm::vec3(0.2126f, 0.7152f, 0.0722f);
constexpr float kOverlapEpsilon = 1e-6f;
constexpr float kTopologyVertexTolerance = 1e-5f;

struct QuantizedPositionKey {
    int64_t x {};
    int64_t y {};
    int64_t z {};

    bool operator==(const QuantizedPositionKey & rhs) const {
        return x == rhs.x && y == rhs.y && z == rhs.z;
    }

    bool operator<(const QuantizedPositionKey & rhs) const {
        if (x != rhs.x) {
            return x < rhs.x;
        }
        if (y != rhs.y) {
            return y < rhs.y;
        }
        return z < rhs.z;
    }
};

struct QuantizedEdgeKey {
    QuantizedPositionKey a {};
    QuantizedPositionKey b {};

    bool operator==(const QuantizedEdgeKey & rhs) const {
        return a == rhs.a && b == rhs.b;
    }
};

struct QuantizedEdgeKeyHash {
    size_t operator()(const QuantizedEdgeKey & key) const {
        size_t seed = 0;
        auto hash_combine = [&](int64_t value) {
            size_t h = std::hash<int64_t>{}(value);
            seed ^= h + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        };

        hash_combine(key.a.x);
        hash_combine(key.a.y);
        hash_combine(key.a.z);
        hash_combine(key.b.x);
        hash_combine(key.b.y);
        hash_combine(key.b.z);
        return seed;
    }
};

FORCEINLINE uint32_t ExpandBits10(uint32_t v) {
    v = (v * 0x00010001u) & 0xFF0000FFu;
    v = (v * 0x00000101u) & 0x0F00F00Fu;
    v = (v * 0x00000011u) & 0xC30C30C3u;
    v = (v * 0x00000005u) & 0x49249249u;
    return v;
}

FORCEINLINE uint32_t MortonCode3D(glm::vec3 p01) {
    glm::vec3 p = glm::clamp(p01, glm::vec3(0.f), glm::vec3(1.f));
    uint32_t x = (uint32_t)std::floor(p.x * 1023.f + 0.5f);
    uint32_t y = (uint32_t)std::floor(p.y * 1023.f + 0.5f);
    uint32_t z = (uint32_t)std::floor(p.z * 1023.f + 0.5f);
    return (ExpandBits10(x) << 2) | (ExpandBits10(y) << 1) | ExpandBits10(z);
}

FORCEINLINE float SafeLuminance(glm::vec3 emissive, bool has_emissive_map) {
    float luma = glm::dot(glm::max(emissive, glm::vec3(0.f)), kLuminanceCoeff);
    if (luma <= 0.f && has_emissive_map) {
        luma = 1.f;
    }
    return luma;
}

FORCEINLINE uint64_t BuildEdgeKey(uint32_t a, uint32_t b) {
    if (a > b) {
        std::swap(a, b);
    }
    return (uint64_t(a) << 32ull) | uint64_t(b);
}

FORCEINLINE QuantizedPositionKey QuantizePosition(glm::vec3 p, float tolerance) {
    float inv_tolerance = 1.f / tolerance;
    return {
        (int64_t)std::llround((double)p.x * (double)inv_tolerance),
        (int64_t)std::llround((double)p.y * (double)inv_tolerance),
        (int64_t)std::llround((double)p.z * (double)inv_tolerance)
    };
}

FORCEINLINE QuantizedEdgeKey BuildQuantizedEdgeKey(glm::vec3 a, glm::vec3 b, float tolerance) {
    QuantizedPositionKey qa = QuantizePosition(a, tolerance);
    QuantizedPositionKey qb = QuantizePosition(b, tolerance);
    if (qb < qa) {
        std::swap(qa, qb);
    }
    return {qa, qb};
}

FORCEINLINE float AABBVolume(const AABB & aabb) {
    if (!aabb.IsValid()) {
        return 0.f;
    }
    glm::vec3 ext = glm::max(aabb.max - aabb.min, glm::vec3(0.f));
    return ext.x * ext.y * ext.z;
}

FORCEINLINE glm::vec3 SafeNormalize(glm::vec3 v, glm::vec3 fallback = glm::vec3(0.f, 0.f, 1.f)) {
    float len = glm::length(v);
    if (len <= 1e-10f) {
        return fallback;
    }
    return v / len;
}

bool TryIntersectEdgeWithScanlineY(glm::vec2 p0, glm::vec2 p1, float scan_y, float & out_x) {
    if (std::abs(p0.y - p1.y) <= kOverlapEpsilon) {
        return false;
    }

    float edge_min_y = std::min(p0.y, p1.y);
    float edge_max_y = std::max(p0.y, p1.y);
    if (scan_y < edge_min_y || scan_y >= edge_max_y) {
        return false;
    }

    float t = (scan_y - p0.y) / (p1.y - p0.y);
    out_x = p0.x + t * (p1.x - p0.x);
    return true;
}

bool HasAnyEmissiveTexelInTriangle(Texture * emissive_map, glm::vec2 uv0, glm::vec2 uv1, glm::vec2 uv2) {
    if (!emissive_map) {
        return true;
    }

    uint32_t width = emissive_map->GetWidth();
    uint32_t height = emissive_map->GetHeight();
    if (width == 0 || height == 0) {
        return true;
    }
    if (emissive_map->GetBinary().empty()) {
        return true;
    }

    auto uv_in_01 = [](glm::vec2 uv) {
        return uv.x >= 0.f && uv.x <= 1.f && uv.y >= 0.f && uv.y <= 1.f;
    };
    if (!uv_in_01(uv0) || !uv_in_01(uv1) || !uv_in_01(uv2)) {
        return true;
    }

    glm::vec2 p0 = uv0 * glm::vec2((float)width, (float)height) - glm::vec2(0.5f);
    glm::vec2 p1 = uv1 * glm::vec2((float)width, (float)height) - glm::vec2(0.5f);
    glm::vec2 p2 = uv2 * glm::vec2((float)width, (float)height) - glm::vec2(0.5f);

    float tri_min_y_f = std::min({p0.y, p1.y, p2.y});
    float tri_max_y_f = std::max({p0.y, p1.y, p2.y});
    int32_t min_y = std::clamp((int32_t)std::ceil(tri_min_y_f), 0, (int32_t)height - 1);
    int32_t max_y = std::clamp((int32_t)std::floor(tri_max_y_f), 0, (int32_t)height - 1);

    if (min_y > max_y) {
        return false;
    }

    for (int32_t y = min_y; y <= max_y; ++y) {
        float scan_y = (float)y;
        std::array<float, 3> intersections {};
        int intersection_count = 0;

        float hit_x;
        if (TryIntersectEdgeWithScanlineY(p0, p1, scan_y, hit_x)) {
            intersections[intersection_count++] = hit_x;
        }
        if (TryIntersectEdgeWithScanlineY(p1, p2, scan_y, hit_x)) {
            intersections[intersection_count++] = hit_x;
        }
        if (TryIntersectEdgeWithScanlineY(p2, p0, scan_y, hit_x)) {
            intersections[intersection_count++] = hit_x;
        }

        if (intersection_count < 2) {
            continue;
        }

        float x_min = intersections[0];
        float x_max = intersections[1];
        if (x_min > x_max) {
            std::swap(x_min, x_max);
        }
        if (intersection_count == 3) {
            float x2 = intersections[2];
            if (x2 < x_min) {
                x_min = x2;
            } else if (x2 > x_max) {
                x_max = x2;
            }
        }

        int32_t x_start = std::clamp((int32_t)std::ceil(x_min), 0, (int32_t)width - 1);
        int32_t x_end = std::clamp((int32_t)std::floor(x_max), 0, (int32_t)width - 1);
        if (x_start > x_end) {
            continue;
        }

        for (int32_t x = x_start; x <= x_end; ++x) {
            auto emissive = emissive_map->Load((uint32_t)x, (uint32_t)y);
            if (emissive.x > 0.f || emissive.y > 0.f || emissive.z > 0.f) {
                return true;
            }
        }
    }

    return false;
}

struct MergePairRecord {
    uint32_t adjacent_cluster_id {UINT32_MAX};
    float cost {std::numeric_limits<float>::infinity()};
};

bool operator < (const MergePairRecord & lhs, const MergePairRecord & rhs) {
    if (lhs.cost != rhs.cost) {
        return lhs.cost < rhs.cost;
    }
    return lhs.adjacent_cluster_id < rhs.adjacent_cluster_id;
}

struct ActiveClusterOrder {
    float total_intensity {0.f};
    uint32_t node_idx {UINT32_MAX};
};

struct ActiveClusterOrderLess {
    bool operator()(const ActiveClusterOrder & lhs, const ActiveClusterOrder & rhs) const {
        if (lhs.total_intensity != rhs.total_intensity) {
            return lhs.total_intensity < rhs.total_intensity;
        }
        return lhs.node_idx < rhs.node_idx;
    }
};

struct BuildRuntimeNode {

    AABB position_aabb {};
    AABB normal_aabb {};

    glm::vec3 weighted_normal_sum {0.f};
    glm::vec3 weighted_normal_sum_2 {0.f};
    glm::vec3 weighted_centroid_sum {0.f};
    float total_intensity {};
    uint32_t triangle_count {};

    union {
        struct {
            uint32_t a, b;
        } childs;
        uint32_t rt_triangle_idx;
    } data {};

    FORCEINLINE bool IsLeaf() const {
        return triangle_count == 1;
    }

    std::set<MergePairRecord> adjacent_merge_pair_records;
};

struct MergeEdge {
    uint32_t a {UINT32_MAX};
    uint32_t b {UINT32_MAX};
    float cost {std::numeric_limits<float>::infinity()};
    bool topology_adjacent {false};
};

struct MergeEdgeLess {
    bool operator()(const MergeEdge & lhs, const MergeEdge & rhs) const {
        if (lhs.cost != rhs.cost) {
            return lhs.cost < rhs.cost;
        }
        if (lhs.a != rhs.a) {
            return lhs.a < rhs.a;
        }
        return lhs.b < rhs.b;
    }
};

FORCEINLINE MergeEdge MakeCanonicalMergeEdge(uint32_t a, uint32_t b, float cost, bool topology_adjacent) {
    if (a > b) {
        std::swap(a, b);
    }
    return {a, b, cost, topology_adjacent};
}

FORCEINLINE ActiveClusterOrder MakeActiveClusterOrder(const BuildRuntimeNode & node, uint32_t node_idx) {
    return {node.total_intensity, node_idx};
}

struct BuildRuntimeTriangle {
    uint32_t primitive_index {};
    glm::vec3 v0 {};
    glm::vec3 v1 {};
    glm::vec3 v2 {};
    float intensity {};
    FORCEINLINE glm::vec3 Centroid() const {
        return (v0 + v1 + v2) / 3.f;
    }
    FORCEINLINE glm::vec3 Normal() const {
        return SafeNormalize(glm::cross(v1 - v0, v2 - v0));
    }
    FORCEINLINE float Area() const {
        return 0.5f * glm::length(glm::cross(v1 - v0, v2 - v0));
    }
    FORCEINLINE AABB LocalAABB() const {
        AABB aabb {};
        aabb.Encapsulate(v0);
        aabb.Encapsulate(v1);
        aabb.Encapsulate(v2);
        return aabb;
    }
};

void BuildRuntimeTriangleList(
    const Geometry & geometry,
    Texture * emissive_map,
    float emissive_luma,
    const MeshLightClusterBuildConfig & config,
    std::vector<BuildRuntimeTriangle> & out_triangles,
    AABB & out_total_aabb,
    float & out_total_intensity
) {
    const auto & vertices = geometry.GetVertexBuffer();
    const auto & indices = geometry.GetIndexBufferRef();
    uint32_t triangle_count = geometry.GetIndexCount() / 3;

    out_triangles.clear();
    out_triangles.reserve(triangle_count);
    out_total_aabb = {};
    out_total_intensity = 0.f;

    for (uint32_t i = 0; i < triangle_count; ++i) {
        uint32_t i0 = indices[i * 3 + 0];
        uint32_t i1 = indices[i * 3 + 1];
        uint32_t i2 = indices[i * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        if (emissive_map && !HasAnyEmissiveTexelInTriangle(emissive_map, vertices[i0].UV, vertices[i1].UV, vertices[i2].UV)) {
            continue;
        }

        glm::vec3 v0 = vertices[i0].Position;
        glm::vec3 v1 = vertices[i1].Position;
        glm::vec3 v2 = vertices[i2].Position;

        BuildRuntimeTriangle tri {};
        tri.primitive_index = i;
        tri.v0 = v0;
        tri.v1 = v1;
        tri.v2 = v2;
        float area = tri.Area();
        float intensity = area * emissive_luma;
        tri.intensity = intensity;

        if (area <= config.min_triangle_area || intensity <= config.min_cluster_intensity) {
            continue;
        }

        out_total_aabb.Encapsulate(tri.LocalAABB());
        out_total_intensity += tri.intensity;
        out_triangles.emplace_back(tri);
    }
}

std::vector<std::unordered_set<uint32_t>> BuildTopologyAdjacency(
    const Geometry & geometry,
    const std::vector<BuildRuntimeTriangle> & filtered_triangles,
    uint32_t source_triangle_count
) {
    std::vector<std::unordered_set<uint32_t>> adjacency(filtered_triangles.size());
    if (filtered_triangles.empty()) {
        return adjacency;
    }

    std::vector<uint32_t> primitive_to_filtered(source_triangle_count, UINT32_MAX);
    for (uint32_t filtered_idx = 0; filtered_idx < (uint32_t)filtered_triangles.size(); ++filtered_idx) {
        uint32_t primitive = filtered_triangles[filtered_idx].primitive_index;
        if (primitive < primitive_to_filtered.size()) {
            primitive_to_filtered[primitive] = filtered_idx;
        }
    }

    const auto & indices = geometry.GetIndexBufferRef();
    std::unordered_map<uint64_t, uint32_t> edge_owner;
    std::unordered_map<QuantizedEdgeKey, uint32_t, QuantizedEdgeKeyHash> geometric_edge_owner;
    edge_owner.reserve(filtered_triangles.size() * 3);
    geometric_edge_owner.reserve(filtered_triangles.size() * 3);
    const auto & vertices = geometry.GetVertexBuffer();

    for (const auto & tri : filtered_triangles) {
        uint32_t primitive = tri.primitive_index;
        if (primitive >= source_triangle_count) {
            continue;
        }
        uint32_t tri_idx = primitive_to_filtered[primitive];
        if (tri_idx == UINT32_MAX) {
            continue;
        }

        uint32_t i0 = indices[primitive * 3 + 0];
        uint32_t i1 = indices[primitive * 3 + 1];
        uint32_t i2 = indices[primitive * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        uint32_t e[3][2] = {{i0, i1}, {i1, i2}, {i2, i0}};
        glm::vec3 edge_positions[3][2] = {
            {vertices[i0].Position, vertices[i1].Position},
            {vertices[i1].Position, vertices[i2].Position},
            {vertices[i2].Position, vertices[i0].Position}
        };

        for (uint32_t edge_i = 0; edge_i < 3; ++edge_i) {
            uint64_t key = BuildEdgeKey(e[edge_i][0], e[edge_i][1]);
            auto iter = edge_owner.find(key);
            if (iter == edge_owner.end()) {
                edge_owner.emplace(key, tri_idx);
            } else {
                uint32_t other = iter->second;
                if (other != tri_idx) {
                    adjacency[tri_idx].insert(other);
                    adjacency[other].insert(tri_idx);
                }
            }

            QuantizedEdgeKey geometric_key = BuildQuantizedEdgeKey(
                edge_positions[edge_i][0],
                edge_positions[edge_i][1],
                kTopologyVertexTolerance
            );
            auto geometric_iter = geometric_edge_owner.find(geometric_key);
            if (geometric_iter == geometric_edge_owner.end()) {
                geometric_edge_owner.emplace(geometric_key, tri_idx);
            } else {
                uint32_t other = geometric_iter->second;
                if (other != tri_idx) {
                    adjacency[tri_idx].insert(other);
                    adjacency[other].insert(tri_idx);
                }
            }
        }
    }

    return adjacency;
}

float EvaluatePairMergeCost(
    const BuildRuntimeNode & a,
    const BuildRuntimeNode & b,
    bool topology_adjacent,
    const MeshLightClusterBuildConfig & config,
    float inv_scene_diag
) {
    AABB merged_position = AABB::Merge(a.position_aabb, b.position_aabb);
    float pos_expand = std::max(0.f, AABBVolume(merged_position) - AABBVolume(a.position_aabb) - AABBVolume(b.position_aabb));

    AABB merged_normal = AABB::Merge(a.normal_aabb, b.normal_aabb);
    float normal_expand = std::max(0.f, AABBVolume(merged_normal) - AABBVolume(a.normal_aabb) - AABBVolume(b.normal_aabb));

    glm::vec3 center_a = a.total_intensity > 0.f ? a.weighted_centroid_sum / a.total_intensity : glm::vec3(0.f);
    glm::vec3 center_b = b.total_intensity > 0.f ? b.weighted_centroid_sum / b.total_intensity : glm::vec3(0.f);
    float centroid_dist = glm::length(center_a - center_b) * inv_scene_diag;

    glm::vec3 normal_a = SafeNormalize(a.weighted_normal_sum);
    glm::vec3 normal_b = SafeNormalize(b.weighted_normal_sum);
    float normal_deviation = std::max(0.f, 1.f - glm::dot(normal_a, normal_b));

    float ia = std::max(a.total_intensity, 1e-10f);
    float ib = std::max(b.total_intensity, 1e-10f);
    float intensity_imbalance = std::abs(std::log2(ia / ib));

    float topology_penalty = topology_adjacent ? 0.f : config.non_topology_penalty_value;

    return
    // FIXME: commented other stuffs out for now for debugging purposes.
        // config.weight_aabb_expand * pos_expand +
        // config.weight_normal_aabb_expand * normal_expand +
        // config.weight_non_topology_penalty * topology_penalty +
        // config.weight_centroid_distance * centroid_dist +
        // config.weight_normal_deviation * normal_deviation +
        config.weight_intensity_imbalance * intensity_imbalance;
}

// Remove all merge pair records between the given node and its neighbors.
void RemoveAllMergePairRecordsAdjacentToNode(
    std::vector<BuildRuntimeNode> & runtime_nodes,
    std::set<MergeEdge, MergeEdgeLess> & merge_pairs,
    uint32_t node_idx
) {
    for (const auto & [neighbor_idx, cost] : runtime_nodes[node_idx].adjacent_merge_pair_records) {
        auto edge = MakeCanonicalMergeEdge(node_idx, neighbor_idx, cost, true);
        merge_pairs.erase(edge);
        // Also, remove the corresponding record in the neighbor node.
        auto & neighbor_records = runtime_nodes[neighbor_idx].adjacent_merge_pair_records;
        auto neighbor_iter = neighbor_records.find(MergePairRecord{node_idx, cost});
        if (neighbor_iter != neighbor_records.end()) {
            neighbor_records.erase(neighbor_iter);
        }
    }
    runtime_nodes[node_idx].adjacent_merge_pair_records.clear();
}

// Insert merge pairs into the global set and the per-node record. (Topologically adjacent)
void InsertMergePairRecord(
    std::vector<BuildRuntimeNode> & runtime_nodes,
    std::set<MergeEdge, MergeEdgeLess> & merge_pairs,
    uint32_t a,
    uint32_t b,
    const MeshLightClusterBuildConfig & config,
    float inv_scene_diag
) {
    if (a == b) return;
    if (a > b) std::swap(a, b);

    float cost = EvaluatePairMergeCost(runtime_nodes[a], runtime_nodes[b], true, config, inv_scene_diag);
    MergeEdge edge = MakeCanonicalMergeEdge(a, b, cost, true);
    merge_pairs.insert(edge);

    runtime_nodes[edge.a].adjacent_merge_pair_records.insert(MergePairRecord{edge.b, edge.cost});
    runtime_nodes[edge.b].adjacent_merge_pair_records.insert(MergePairRecord{edge.a, edge.cost});
}

MergeEdge FindBestTopologyMergeEdge(const std::set<MergeEdge, MergeEdgeLess> & merge_pairs) {
    if (!merge_pairs.empty()) {
        return *merge_pairs.begin();
    }
    return {};
}

std::vector<uint32_t> BuildSnapshotTargets(uint32_t n) {
    std::vector<uint32_t> targets;
    if (n == 0) {
        return targets;
    }
    uint32_t t = n;
    while (true) {
        targets.push_back(t);
        if (t == 1) {
            break;
        }
        t = std::max(1u, t / 2u);
    }
    return targets;
}

std::vector<uint32_t> SnapshotLevelRuntimeNodes(
    const std::set<ActiveClusterOrder, ActiveClusterOrderLess> & active_clusters_by_intensity
) {
    std::vector<uint32_t> level;
    level.reserve(active_clusters_by_intensity.size());
    for (const auto & active : active_clusters_by_intensity) {
        level.push_back(active.node_idx);
    }
    return level;
}

MergeEdge FindFallbackMergeEdge(
    const std::vector<BuildRuntimeNode> & runtime_nodes,
    const std::set<ActiveClusterOrder, ActiveClusterOrderLess> & active_clusters_by_intensity,
    const MeshLightClusterBuildConfig & config,
    float inv_scene_diag
) {
    uint32_t search_count = std::min<uint32_t>((uint32_t)active_clusters_by_intensity.size(), (uint32_t)std::max(CVar_LightClusterBuildFallbackSearchN.Get(), 1));
    MergeEdge best {};
    std::unordered_set<uint64_t> visited_pairs;
    visited_pairs.reserve((size_t)search_count * std::max<size_t>(active_clusters_by_intensity.size(), 1));

    std::vector<uint32_t> weakest_active_nodes;
    weakest_active_nodes.reserve(search_count);
    auto weakest_iter = active_clusters_by_intensity.begin();
    for (uint32_t i = 0; i < search_count && weakest_iter != active_clusters_by_intensity.end(); ++i, ++weakest_iter) {
        weakest_active_nodes.push_back(weakest_iter->node_idx);
    }

    // Fallback intentionally starts from the weakest active clusters first.
    // This keeps disconnected low-power islands from stalling the agglomeration process,
    // while still allowing them to merge with any active cluster in the hierarchy.
    for (uint32_t a : weakest_active_nodes) {
        for (auto b : active_clusters_by_intensity) {
            if (a == b.node_idx) {
                continue;
            }
            uint64_t pair_key = BuildEdgeKey(a, b.node_idx);
            if (!visited_pairs.insert(pair_key).second) {
                continue;
            }
            float cost = EvaluatePairMergeCost(runtime_nodes[a], runtime_nodes[b.node_idx], false, config, inv_scene_diag);
            if (cost < best.cost) {
                best = MakeCanonicalMergeEdge(a, b.node_idx, cost, false);
            }
        }
    }
    return best;
}

} // namespace

std::optional<MeshLightClusterHierarchy> BuildMeshLightClusterHierarchy(
    const Geometry & geometry,
    const Material & material,
    const MeshLightClusterBuildConfig & config
) {
    MeshLightClusterHierarchy hierarchy {};

    if (geometry.GetIndexCount() < 3 || !material.IsEmissive()) {
        return hierarchy;
    }

    auto packed_material = material.PackMaterialHeader();
    float emissive_luma = SafeLuminance(packed_material.Emissive, material.GetEmissiveTexture() != nullptr);
    if (emissive_luma <= 0.f) {
        return hierarchy;
    }

    AABB total_aabb {};
    printf("StartBuilding Lit Triangles!\n");
    std::vector<BuildRuntimeTriangle> rt_triangles;
    float total_intensity {};
    BuildRuntimeTriangleList(
        geometry,
        material.GetEmissiveTexture(),
        emissive_luma,
        config,
        rt_triangles,
        total_aabb,
        total_intensity
    );
    printf("Total valid emissive triangles: %zu, intensity: %f\n", rt_triangles.size(), total_intensity);

    if (rt_triangles.empty()) {
        // No valid emissive triangles after filtering, return empty hierarchy.
        return hierarchy;
    }

    // Build topology adjacency over filtered triangle indices.
    mi_check(geometry.IsTriangularGeometry(), "BuildMeshLightClusterHierarchy currently only supports triangular meshes.");
    auto triangle_topology_adjacency = BuildTopologyAdjacency(geometry, rt_triangles, geometry.GetIndexCount() / 3);
    // Triangle index -> runtime leaf node indirection
    auto triangle_to_rt_leaf_node = std::unordered_map<uint32_t, uint32_t>();

    auto leaf_count = (uint32_t)rt_triangles.size();
    printf("Topology built! start merging with %u leaves...\n", leaf_count);

    // Prepare leaf runtime nodes
    std::vector<BuildRuntimeNode> runtime_nodes;
    runtime_nodes.reserve(leaf_count * 2 - 1);

    std::set<ActiveClusterOrder, ActiveClusterOrderLess> active_clusters_by_intensity;

    for (uint32_t rt_tri_index = 0; rt_tri_index < leaf_count; ++rt_tri_index) {
        // Initialize runtime nodes.
        BuildRuntimeNode rt {};
        const auto tri = rt_triangles[rt_tri_index];
        rt.position_aabb = tri.LocalAABB();
        rt.normal_aabb = {};
        rt.normal_aabb.Encapsulate(tri.Normal());
        rt.weighted_normal_sum = tri.Normal() * tri.intensity;
        rt.weighted_normal_sum_2 = tri.Normal() * tri.Normal() * tri.intensity;
        rt.weighted_centroid_sum = tri.Centroid() * tri.intensity;
        rt.total_intensity = tri.intensity;
        rt.triangle_count = 1;
        rt.data.rt_triangle_idx = rt_tri_index;
        rt.data.childs.b = UINT32_MAX;

        // Insert rt node
        auto rt_node_idx = (uint32_t)runtime_nodes.size();
        runtime_nodes.push_back(std::move(rt));
        // Store indirection
        triangle_to_rt_leaf_node[tri.primitive_index] = rt_tri_index;
        // Build active rt node sets.
        active_clusters_by_intensity.insert(MakeActiveClusterOrder(runtime_nodes[rt_node_idx], rt_node_idx));
    }

    // Only topology-adjacent merge candidates live in the ordered set.
    // Morton supplementation is intentionally excluded from long-lived pair maintenance,
    // and is reserved for separate initialization/fallback logic.
    std::set<MergeEdge, MergeEdgeLess> merge_pairs;

    AABB scene_aabb = total_aabb;
    float scene_diag = glm::length(glm::max(scene_aabb.max - scene_aabb.min, glm::vec3(1e-6f)));
    float inv_scene_diag = 1.f / std::max(scene_diag, 1e-6f);

    // Initialize the global merge pair candidate set.
    for (uint32_t u = 0; u < leaf_count; u++) {
        auto tri_idx = runtime_nodes[u].data.rt_triangle_idx;
        for (uint32_t adj_tri_idx : triangle_topology_adjacency[tri_idx]) {
            // Filtered triangles have identical index to their corresponding rt nodes. No mapping needed here.
            auto v = adj_tri_idx;
            if (u >= v) {
                // Only insert each pair once.
                continue;
            }
            InsertMergePairRecord(runtime_nodes, merge_pairs, u, v, config, inv_scene_diag);
        }
    }

    // Capture snapshots at N, N/2, N/4 ... 1 active clusters.
    auto snapshot_targets = BuildSnapshotTargets(leaf_count);
    uint32_t snapshot_cursor = 0;

    std::vector<std::vector<uint32_t>> level_snapshots;
    level_snapshots.emplace_back(SnapshotLevelRuntimeNodes(active_clusters_by_intensity));
    snapshot_cursor = 1;

    while (active_clusters_by_intensity.size() > 1) {

        if (active_clusters_by_intensity.size() % 5000 == 0) {
            printf("BuildMeshLightClusterHierarchy(): Large light mesh detected. Clustering... %zu clusters.\n", active_clusters_by_intensity.size());
        }
        // Regular path only considers topology-adjacent pairs.
        // If topology is broken and the set contains no such pair, we fall back to the
        // documented "first N weakest clusters" search below.
        MergeEdge best_edge = FindBestTopologyMergeEdge(merge_pairs);
        bool found = best_edge.a != UINT32_MAX && best_edge.b != UINT32_MAX;

        if (!found) {
            best_edge = FindFallbackMergeEdge(runtime_nodes, active_clusters_by_intensity, config, inv_scene_diag);
            found = best_edge.a != UINT32_MAX && best_edge.b != UINT32_MAX;
        }

        if (!found) { // No valid merge edge found, should only happen in very degenerate cases.
            // Exit immediately and report
            MI_WARN("Failed to build light cluster hierarchy: no valid merge pairs found.");
            return std::nullopt;
        }

        // Merge nodes a and b into a new node, and rewire neighbors.
        uint32_t a = best_edge.a;
        uint32_t b = best_edge.b;

        // Build merged runtime node.
        uint32_t merged_rt_node = (uint32_t)runtime_nodes.size();
        {
            BuildRuntimeNode merged_rt {};
            merged_rt.position_aabb = AABB::Merge(runtime_nodes[a].position_aabb, runtime_nodes[b].position_aabb);
            merged_rt.normal_aabb = AABB::Merge(runtime_nodes[a].normal_aabb, runtime_nodes[b].normal_aabb);
            merged_rt.weighted_normal_sum = runtime_nodes[a].weighted_normal_sum + runtime_nodes[b].weighted_normal_sum;
            merged_rt.weighted_normal_sum_2 = runtime_nodes[a].weighted_normal_sum_2 + runtime_nodes[b].weighted_normal_sum_2;
            merged_rt.weighted_centroid_sum = runtime_nodes[a].weighted_centroid_sum + runtime_nodes[b].weighted_centroid_sum;
            merged_rt.total_intensity = runtime_nodes[a].total_intensity + runtime_nodes[b].total_intensity;
            merged_rt.triangle_count = runtime_nodes[a].triangle_count + runtime_nodes[b].triangle_count;
            merged_rt.data.childs.a = a;
            merged_rt.data.childs.b = b;

            // printf("num_tr: %d %d %d\n", runtime_nodes[a].triangle_indices.size(), runtime_nodes[b].triangle_indices.size(), merged_rt.triangle_indices.capacity());
            printf("cost: %f, topology_adj: %d, lc: %d, rc: %d\n", best_edge.cost, best_edge.topology_adjacent, runtime_nodes[a].triangle_count, runtime_nodes[b].triangle_count);

            runtime_nodes.push_back(std::move(merged_rt));
        }

        // Maintain active clusters sorted by intensity for fallback search.
        auto xx = active_clusters_by_intensity.erase(MakeActiveClusterOrder(runtime_nodes[a], a));
        auto yy = active_clusters_by_intensity.erase(MakeActiveClusterOrder(runtime_nodes[b], b));
        if (xx + yy != 2) {
            puts("what the fuck?");
        }
        active_clusters_by_intensity.insert(MakeActiveClusterOrder(runtime_nodes[merged_rt_node], merged_rt_node));

        // Gather the topology neighborhood before removing old pair records.
        // Since merge_pairs only tracks topology-adjacent pairs, only those edges need
        // to be rewired and reinserted after the merge.
        std::vector<uint32_t> union_adj;
        for (auto e : runtime_nodes[a].adjacent_merge_pair_records) {
            union_adj.push_back(e.adjacent_cluster_id);
        }
        for (auto e : runtime_nodes[b].adjacent_merge_pair_records) {
            union_adj.push_back(e.adjacent_cluster_id);
        }
        std::sort(union_adj.begin(), union_adj.end());
        union_adj.erase(std::unique(union_adj.begin(), union_adj.end()), union_adj.end());

        // Remove all merge pair records between a and its neighbors, and b and its neighbors, to avoid stale records after the merge.
        RemoveAllMergePairRecordsAdjacentToNode(runtime_nodes, merge_pairs, a);
        RemoveAllMergePairRecordsAdjacentToNode(runtime_nodes, merge_pairs, b);

        // Okay, insert new topology edges between the merged node and the union of the old neighborhoods of a and b.
        for (auto v : union_adj) {
            if (v == a || v == b) {
                continue;
            }
            InsertMergePairRecord(runtime_nodes, merge_pairs, merged_rt_node, v, config, inv_scene_diag);
        }

        // Done.

        // Capture snapshot when crossing target count exactly.
        while (snapshot_cursor < snapshot_targets.size() && active_clusters_by_intensity.size() == snapshot_targets[snapshot_cursor]) {
            level_snapshots.emplace_back(SnapshotLevelRuntimeNodes(active_clusters_by_intensity));
            ++snapshot_cursor;
        }
    }

    auto rng32 = std::mt19937(std::random_device{}());

    std::unordered_map<uint32_t, MeshLightClusterChild> rt_node_to_hierarchy_node;
    // Build final hierarchy structures
    std::function<MeshLightClusterChild(uint32_t)> BuildFinalHierarchyNode = [&](uint32_t rt_node_idx) {
        auto & rt_node = runtime_nodes[rt_node_idx];
        if (rt_node.IsLeaf()) {
            // Build a leaf triangle and return the index.
            auto u = hierarchy.triangles.size();
            rt_node_to_hierarchy_node[rt_node_idx] = {true, (uint32_t)u};
            auto tri_idx = rt_node.data.rt_triangle_idx;
            hierarchy.triangles.push_back({rt_triangles[tri_idx].primitive_index});
            return MeshLightClusterChild{true, (uint32_t)u};
        }
        auto lc = BuildFinalHierarchyNode(rt_node.data.childs.a);
        auto rc = BuildFinalHierarchyNode(rt_node.data.childs.b);
        auto u = hierarchy.nodes.size();
        rt_node_to_hierarchy_node[rt_node_idx] = {false, (uint32_t)u};
        MeshLightClusterNode node {};
        MeshLightClusterHeader header {};
        node.L = lc;
        node.R = rc;
        node.L_Weight = runtime_nodes[rt_node.data.childs.a].total_intensity;
        node.R_Weight = runtime_nodes[rt_node.data.childs.b].total_intensity;
        hierarchy.nodes.push_back(node);
        header.Hash = rng32();
        header.LocalAABBMin = rt_node.position_aabb.min;
        header.LocalAABBMax = rt_node.position_aabb.max;
        header.TotalIntensity = rt_node.total_intensity;
        header.WeightedNormal = SafeNormalize(rt_node.weighted_normal_sum);
        auto diffs = rt_node.weighted_normal_sum_2 - (rt_node.weighted_normal_sum * rt_node.weighted_normal_sum);
        header.WeightedNormalVariance = glm::dot(diffs, diffs);
        hierarchy.headers.push_back(header);
        return MeshLightClusterChild{false, (uint32_t)u};
    };

    auto root = BuildFinalHierarchyNode((uint32_t)runtime_nodes.size() - 1);
    hierarchy.total_intensity = runtime_nodes.back().total_intensity;
    hierarchy.root_node = root; // <- this can be a triangle index if the hierarchy degenerates to a single triangle
    // Record levels
    for (const auto & level : level_snapshots) {
        std::vector<MeshLightClusterChild> level_node_indices;
        level_node_indices.reserve(level.size());
        for (uint32_t rt_node_idx : level) {
            level_node_indices.push_back(rt_node_to_hierarchy_node[rt_node_idx]);
        }
        hierarchy.levels.push_back({std::move(level_node_indices)});
    }

    return hierarchy;
}

MI_NAMESPACE_END
