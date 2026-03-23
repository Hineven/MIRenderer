/*
 * Created: 2026/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <renderer/r_light_cluster_hiearchy.h>
#include <renderer/mi_texture.h>
#include <renderer/util/kd_tree.h>

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

bool HasAnyEmissiveTexelInTriangle(
    Texture * emissive_map,
    glm::vec2 uv0,
    glm::vec2 uv1,
    glm::vec2 uv2,
    float & out_avg_emissive_power
) {
    out_avg_emissive_power = 1.f;
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
        out_avg_emissive_power = 0.f;
        return false;
    }

    float luminance_sum = 0.f;
    uint32_t sampled_texel_count = 0;
    bool has_emissive_texel = false;

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
            float texel_power = glm::dot(glm::max(xyz(emissive), glm::vec3(0.f)), kLuminanceCoeff);
            luminance_sum += texel_power;
            ++sampled_texel_count;
            if (emissive.x > 0.f || emissive.y > 0.f || emissive.z > 0.f) {
                has_emissive_texel = true;
            }
        }
    }

    if (sampled_texel_count > 0) {
        out_avg_emissive_power = luminance_sum / (float)sampled_texel_count;
    } else {
        out_avg_emissive_power = 0.f;
    }

    return has_emissive_texel;
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
    float total_area {};
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

    std::unordered_set<uint32_t> topology_neighbors;
};

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

// KD-Tree payload for cluster centroid queries.
// cluster_id is used by KD-Tree for O(1) node lookup without external mapping.
struct ClusterNode {
    float x, y, z;
    uint32_t cluster_id;

    ClusterNode() = default;
    ClusterNode(glm::vec3 centroid, uint32_t node_idx)
        : x(centroid.x), y(centroid.y), z(centroid.z), cluster_id(node_idx) {}
};

using ClusterKDTree = KDTree3D<ClusterNode>;

// Helper to compute cluster centroid from weighted centroid sum
FORCEINLINE glm::vec3 ComputeClusterCentroid(const BuildRuntimeNode& node) {
    if (node.total_intensity <= 0.f) {
        return (node.position_aabb.min + node.position_aabb.max) * 0.5f;
    }
    return node.weighted_centroid_sum / node.total_intensity;
}

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

        float avg_emissive_power = 1.f;
        if (emissive_map && !HasAnyEmissiveTexelInTriangle(
            emissive_map,
            vertices[i0].UV,
            vertices[i1].UV,
            vertices[i2].UV,
            avg_emissive_power
        )) {
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
        float intensity = area * emissive_luma * avg_emissive_power;
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
    int cnt_n3 = 0;
    for (auto e : adjacency) {
        if (e.size() != 3) cnt_n3++;
    }

    return adjacency;
}

FORCEINLINE float AABBSurfaceArea(const AABB & aabb) {
    if (!aabb.IsValid()) {
        return 0.f;
    }
    glm::vec3 ext = glm::max(aabb.max - aabb.min, glm::vec3(0.f));
    return 2.f * (ext.x * ext.y + ext.y * ext.z + ext.z * ext.x);
}

float EvaluatePairMergeCost(const BuildRuntimeNode & a, const BuildRuntimeNode & b) {
    AABB merged_position = AABB::Merge(a.position_aabb, b.position_aabb);
    AABB merged_normal = AABB::Merge(a.normal_aabb, b.normal_aabb);

    float merged_position_area = std::max(AABBSurfaceArea(merged_position), 1e-8f);
    float merged_total_area = std::max(a.total_area + b.total_area, 1e-8f);
    float avg_area_power_density = (a.total_intensity + b.total_intensity) / merged_total_area;

    float merged_normal_area = AABBSurfaceArea(merged_normal);
    float normal_area_sum = AABBSurfaceArea(a.normal_aabb) + AABBSurfaceArea(b.normal_aabb);
    float normal_expand = normal_area_sum > 1e-8f ? (merged_normal_area / normal_area_sum) : 1.f;

    return merged_position_area * avg_area_power_density * std::max(normal_expand, 1e-4f);
}

struct MergeCandidate {
    uint32_t partner {UINT32_MAX};
    float cost {std::numeric_limits<float>::infinity()};
    bool topology_adjacent {false};
};

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

std::optional<MergeCandidate> FindBestMergeForWeakestCluster(
    uint32_t weakest_node,
    const std::vector<BuildRuntimeNode> & runtime_nodes,
    const ClusterKDTree * kdtree,
    uint32_t kdtree_k
) {

    MergeCandidate best {};

    for (uint32_t v : runtime_nodes[weakest_node].topology_neighbors) {
        float cost = EvaluatePairMergeCost(runtime_nodes[weakest_node], runtime_nodes[v]);
        if (cost < best.cost) {
            best.partner = v;
            best.cost = cost;
            best.topology_adjacent = true;
        }
    }

    if (best.partner != UINT32_MAX) {
        return best;
    }

    // Fallback: use KD-Tree to find nearest clusters by centroid distance.
    // Query the k nearest clusters and evaluate merge cost only with those.
    glm::vec3 weakest_centroid = ComputeClusterCentroid(runtime_nodes[weakest_node]);
    ClusterNode query_pt{weakest_centroid, weakest_node};

    auto knn_results = kdtree->KNN(query_pt, static_cast<int>(kdtree_k) + 1);
    for (const auto & knn : knn_results) {
        uint32_t v = kdtree->GetPoint(knn.point_index).cluster_id;
        if (v == weakest_node) {
            continue;
        }
        float cost = EvaluatePairMergeCost(runtime_nodes[weakest_node], runtime_nodes[v]);
        if (cost < best.cost) {
            best.partner = v;
            best.cost = cost;
            best.topology_adjacent = false;
        }
    }

    if (best.partner == UINT32_MAX) {
        return std::nullopt;
    }
    return best;
}

void RewireTopologyNeighborsAfterMerge(
    std::vector<BuildRuntimeNode> & runtime_nodes,
    uint32_t a,
    uint32_t b,
    uint32_t merged_node
) {
    std::unordered_set<uint32_t> merged_neighbors;
    merged_neighbors.reserve(runtime_nodes[a].topology_neighbors.size() + runtime_nodes[b].topology_neighbors.size());

    for (uint32_t n : runtime_nodes[a].topology_neighbors) {
        if (n == a || n == b) continue;
        merged_neighbors.insert(n);
        runtime_nodes[n].topology_neighbors.erase(a);
    }
    for (uint32_t n : runtime_nodes[b].topology_neighbors) {
        if (n == a || n == b) continue;
        merged_neighbors.insert(n);
        runtime_nodes[n].topology_neighbors.erase(b);
    }

    runtime_nodes[merged_node].topology_neighbors = merged_neighbors;
    for(auto n : merged_neighbors) {
        runtime_nodes[n].topology_neighbors.insert(merged_node);
    }

    // They are no longer needed
    runtime_nodes[a].topology_neighbors.clear();
    runtime_nodes[b].topology_neighbors.clear();
}

} // namespace

std::optional<MeshLightClusterHierarchy> BuildMeshLightClusterHierarchy(
    const Geometry & geometry,
    const Material & material,
    const MeshLightClusterBuildConfig & config
) {
    if (geometry.GetIndexCount() > 30000) {
        // TODO cache preprocessed data in disk
        MI_INFO("BuildMeshLightClusterHierarchy: Large geometry encountered. Triangle count: {}", geometry.GetIndexCount() / 3);
    }
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

    if (rt_triangles.empty()) {
        return hierarchy;
    }

    mi_check(geometry.IsTriangularGeometry(), "BuildMeshLightClusterHierarchy currently only supports triangular meshes.");
    auto triangle_topology_adjacency = BuildTopologyAdjacency(geometry, rt_triangles, geometry.GetIndexCount() / 3);

    auto leaf_count = (uint32_t)rt_triangles.size();
    std::vector<BuildRuntimeNode> runtime_nodes;
    runtime_nodes.reserve(leaf_count * 2 - 1);

    std::set<ActiveClusterOrder, ActiveClusterOrderLess> active_clusters_by_intensity;

    // KD-Tree for spatial queries during fallback merge
    constexpr uint32_t kFallbackKNN = 16;
    ClusterKDTree cluster_kdtree;

    std::unordered_map<uint32_t, uint32_t> rt_node_to_kdtree_node;
    for (uint32_t rt_tri_index = 0; rt_tri_index < leaf_count; ++rt_tri_index) {
        BuildRuntimeNode rt {};
        const auto & tri = rt_triangles[rt_tri_index];
        glm::vec3 tri_normal = tri.Normal();
        float tri_area = tri.Area();

        rt.position_aabb = tri.LocalAABB();
        rt.normal_aabb = {};
        rt.normal_aabb.Encapsulate(tri_normal);
        rt.weighted_normal_sum = tri_normal * tri.intensity;
        rt.weighted_normal_sum_2 = tri_normal * tri_normal * tri.intensity;
        rt.weighted_centroid_sum = tri.Centroid() * tri.intensity;
        rt.total_intensity = tri.intensity;
        rt.total_area = tri_area;
        rt.triangle_count = 1;
        rt.data.rt_triangle_idx = rt_tri_index;
        rt.topology_neighbors = triangle_topology_adjacency[rt_tri_index];

        uint32_t rt_node_idx = (uint32_t)runtime_nodes.size();
        runtime_nodes.push_back(std::move(rt));
        active_clusters_by_intensity.insert(MakeActiveClusterOrder(runtime_nodes[rt_node_idx], rt_node_idx));

        // Add to KD-Tree
        glm::vec3 centroid = ComputeClusterCentroid(runtime_nodes[rt_node_idx]);
        ClusterNode kd_point{centroid, rt_node_idx};
        auto kd_node = cluster_kdtree.Insert(kd_point);
        rt_node_to_kdtree_node[rt_node_idx] = kd_node;
    }

    auto snapshot_targets = BuildSnapshotTargets(leaf_count);
    uint32_t snapshot_cursor = 0;
    std::vector<std::vector<uint32_t>> level_snapshots;
    level_snapshots.emplace_back(SnapshotLevelRuntimeNodes(active_clusters_by_intensity));
    snapshot_cursor = 1;


    while (active_clusters_by_intensity.size() > 1) {
        auto remaining = active_clusters_by_intensity.size();
        uint32_t weakest = active_clusters_by_intensity.begin()->node_idx;
        auto best_opt = FindBestMergeForWeakestCluster(weakest, runtime_nodes, &cluster_kdtree, kFallbackKNN);
        if (!best_opt.has_value()) {
            MI_WARN("Failed to build light cluster hierarchy: no valid merge partner for weakest active cluster.");
            return std::nullopt;
        }

        uint32_t a = weakest;
        uint32_t b = best_opt->partner;
        uint32_t merged_rt_node = (uint32_t)runtime_nodes.size();

        {
            BuildRuntimeNode merged_rt {};
            merged_rt.position_aabb = AABB::Merge(runtime_nodes[a].position_aabb, runtime_nodes[b].position_aabb);
            merged_rt.normal_aabb = AABB::Merge(runtime_nodes[a].normal_aabb, runtime_nodes[b].normal_aabb);
            merged_rt.weighted_normal_sum = runtime_nodes[a].weighted_normal_sum + runtime_nodes[b].weighted_normal_sum;
            merged_rt.weighted_normal_sum_2 = runtime_nodes[a].weighted_normal_sum_2 + runtime_nodes[b].weighted_normal_sum_2;
            merged_rt.weighted_centroid_sum = runtime_nodes[a].weighted_centroid_sum + runtime_nodes[b].weighted_centroid_sum;
            merged_rt.total_intensity = runtime_nodes[a].total_intensity + runtime_nodes[b].total_intensity;
            merged_rt.total_area = runtime_nodes[a].total_area + runtime_nodes[b].total_area;
            merged_rt.triangle_count = runtime_nodes[a].triangle_count + runtime_nodes[b].triangle_count;
            merged_rt.data.childs.a = a;
            merged_rt.data.childs.b = b;

            runtime_nodes.push_back(std::move(merged_rt));
        }

        RewireTopologyNeighborsAfterMerge(runtime_nodes, a, b, merged_rt_node);

        auto x = active_clusters_by_intensity.erase(MakeActiveClusterOrder(runtime_nodes[a], a));
        auto y = active_clusters_by_intensity.erase(MakeActiveClusterOrder(runtime_nodes[b], b));
        mi_check(x+y == 2, "Failed to erase merged clusters from active set.");

        active_clusters_by_intensity.insert(MakeActiveClusterOrder(runtime_nodes[merged_rt_node], merged_rt_node));

        // Update KD-Tree: lazy remove old clusters and insert new merged cluster.
        {
            cluster_kdtree.Remove(rt_node_to_kdtree_node[a]);
            cluster_kdtree.Remove(rt_node_to_kdtree_node[b]);

            glm::vec3 merged_centroid = ComputeClusterCentroid(runtime_nodes[merged_rt_node]);
            ClusterNode merged_kd_point{merged_centroid, merged_rt_node};
            auto merged_kd_node = cluster_kdtree.Insert(merged_kd_point);
            rt_node_to_kdtree_node[merged_rt_node] = merged_kd_node;
        }

        while (snapshot_cursor < snapshot_targets.size() && active_clusters_by_intensity.size() == snapshot_targets[snapshot_cursor]) {
            level_snapshots.emplace_back(SnapshotLevelRuntimeNodes(active_clusters_by_intensity));
            ++snapshot_cursor;
        }
    }

    auto rng32 = std::mt19937(std::random_device{}());

    std::unordered_map<uint32_t, MeshLightClusterChild> rt_node_to_hierarchy_node;
    std::function<MeshLightClusterChild(uint32_t)> BuildFinalHierarchyNode = [&](uint32_t rt_node_idx) {
        auto & rt_node = runtime_nodes[rt_node_idx];
        if (rt_node.IsLeaf()) {
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
        float L_UnnormalizedWeight = runtime_nodes[rt_node.data.childs.a].total_intensity;
        float R_UnnormalizedWeight = runtime_nodes[rt_node.data.childs.b].total_intensity;
        float L_Scale = asdfasfsda
        hierarchy.nodes.push_back(node);
        header.Hash = rng32();
        header.LocalAABBMin = rt_node.position_aabb.min;
        header.LocalAABBMax = rt_node.position_aabb.max;
        header.TotalIntensity = rt_node.total_intensity;
        header.WeightedNormal = SafeNormalize(rt_node.weighted_normal_sum);
        if (rt_node.total_intensity > 1e-12f) {
            float inv_total_intensity = 1.0f / rt_node.total_intensity;
            glm::vec3 mean = rt_node.weighted_normal_sum * inv_total_intensity;
            glm::vec3 mean2 = rt_node.weighted_normal_sum_2 * inv_total_intensity;
            glm::vec3 diffs = glm::max(mean2 - (mean * mean), glm::vec3(0.0f));
            header.WeightedNormalVariance = glm::dot(diffs, diffs);
        } else {
            header.WeightedNormalVariance = 0.0f;
        }
        header.ScaleIntensityMultiplier = rt_node.scale_intensity_multiplier;

        hierarchy.headers.push_back(header);
        return MeshLightClusterChild{false, (uint32_t)u};
    };

    auto root = BuildFinalHierarchyNode((uint32_t)runtime_nodes.size() - 1);
    hierarchy.total_intensity = runtime_nodes.back().total_intensity;
    hierarchy.root_node = root;

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
