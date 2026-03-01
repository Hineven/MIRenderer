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
#include <queue>
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
 * 2) Build topology adjacency (shared edge) and supplement graph links from Morton neighbors.
 * 3) Iteratively merge the best pair (greedy agglomerative clustering) using a multi-objective cost.
 * 4) Store the merge process as a tree.
 * 5) Capture independent LOD snapshots when active cluster count reaches N, N/2, N/4 ... 1.
 *
 * Why this shape:
 * - Tree storage keeps total node count O(N), avoiding O(N log N) duplication in hierarchy storage.
 * - Snapshot levels still provide easy GPU upload and budgeted runtime selection.
 */

constexpr glm::vec3 kLuminanceCoeff = glm::vec3(0.2126f, 0.7152f, 0.0722f);
constexpr float kOverlapEpsilon = 1e-6f;

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

struct BuildRuntimeNode {
    bool active {true};

    AABB position_aabb {};
    AABB normal_aabb {};

    glm::vec3 weighted_normal_sum {0.f};
    glm::vec3 weighted_centroid_sum {0.f};
    float total_intensity {};

    std::vector<uint32_t> triangle_indices;

    std::unordered_set<uint32_t> augmented_neighbors;
    std::unordered_set<uint32_t> topology_neighbors;
};

struct MergeEdge {
    uint32_t a {UINT32_MAX};
    uint32_t b {UINT32_MAX};
    float cost {std::numeric_limits<float>::infinity()};
    bool topology_adjacent {false};
};

struct MergeEdgeGreater {
    bool operator()(const MergeEdge & lhs, const MergeEdge & rhs) const {
        return lhs.cost > rhs.cost;
    }
};

void BuildTriangleList(
    const Geometry & geometry,
    Texture * emissive_map,
    float emissive_luma,
    const MeshLightClusterBuildConfig & config,
    std::vector<MeshLightTriangle> & out_triangles,
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

        glm::vec3 cross_n = glm::cross(v1 - v0, v2 - v0);
        float area2 = glm::length(cross_n);
        float area = area2 * 0.5f;
        if (area <= config.min_triangle_area) {
            continue;
        }

        float intensity = area * emissive_luma;
        if (intensity <= config.min_cluster_intensity) {
            continue;
        }

        MeshLightTriangle tri {};
        tri.primitive_index = i;
        tri.v0 = v0;
        tri.v1 = v1;
        tri.v2 = v2;
        tri.area = area;
        tri.intensity = intensity;
        tri.centroid = (v0 + v1 + v2) * (1.f / 3.f);
        tri.local_aabb = {};
        tri.local_aabb.Encapsulate(v0);
        tri.local_aabb.Encapsulate(v1);
        tri.local_aabb.Encapsulate(v2);
        tri.normal = area2 > 0.f ? (cross_n / area2) : glm::vec3(0.f, 0.f, 1.f);

        out_total_aabb.Encapsulate(tri.local_aabb);
        out_total_intensity += tri.intensity;
        out_triangles.emplace_back(tri);
    }
}

std::vector<uint32_t> BuildSortedTriangleIndicesByMorton(const std::vector<MeshLightTriangle> & triangles, const AABB & total_aabb) {
    std::vector<uint32_t> sorted_indices(triangles.size());
    std::iota(sorted_indices.begin(), sorted_indices.end(), 0u);

    glm::vec3 extent = glm::max(total_aabb.max - total_aabb.min, glm::vec3(1e-6f));
    std::sort(sorted_indices.begin(), sorted_indices.end(), [&](uint32_t a, uint32_t b) {
        glm::vec3 pa = (triangles[a].centroid - total_aabb.min) / extent;
        glm::vec3 pb = (triangles[b].centroid - total_aabb.min) / extent;
        return MortonCode3D(pa) < MortonCode3D(pb);
    });

    return sorted_indices;
}

std::vector<std::unordered_set<uint32_t>> BuildTopologyAdjacency(
    const Geometry & geometry,
    const std::vector<MeshLightTriangle> & filtered_triangles,
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
    edge_owner.reserve(filtered_triangles.size() * 3);

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
        }
    }

    return adjacency;
}

void AugmentAdjacencyByMortonWindow(
    std::vector<std::unordered_set<uint32_t>> & adjacency,
    const std::vector<uint32_t> & sorted_indices,
    uint32_t morton_neighbor_window
) {
    if (sorted_indices.empty() || morton_neighbor_window == 0) {
        return;
    }

    for (uint32_t sorted_pos = 0; sorted_pos < (uint32_t)sorted_indices.size(); ++sorted_pos) {
        uint32_t tri_a = sorted_indices[sorted_pos];
        uint32_t begin = sorted_pos > morton_neighbor_window ? sorted_pos - morton_neighbor_window : 0;
        uint32_t end = std::min<uint32_t>((uint32_t)sorted_indices.size(), sorted_pos + morton_neighbor_window + 1);
        for (uint32_t pos = begin; pos < end; ++pos) {
            if (pos == sorted_pos) {
                continue;
            }
            uint32_t tri_b = sorted_indices[pos];
            if (tri_a == tri_b) {
                continue;
            }
            adjacency[tri_a].insert(tri_b);
            adjacency[tri_b].insert(tri_a);
        }
    }
}

MeshLightClusterHeader BuildClusterHeaderFromTriangleMembers(
    const std::vector<MeshLightTriangle> & triangles,
    std::span<const uint32_t> triangle_members,
    uint32_t level,
    uint32_t triangle_offset
) {
    MeshLightClusterHeader header {};
    header.level = level;
    header.triangle_offset = triangle_offset;
    header.triangle_count = (uint32_t)triangle_members.size();

    glm::vec3 weighted_normal_acc = glm::vec3(0.f);
    float total_intensity = 0.f;
    header.local_aabb = {};

    for (uint32_t tri_idx : triangle_members) {
        const auto & tri = triangles[tri_idx];
        header.local_aabb.Encapsulate(tri.local_aabb);
        weighted_normal_acc += tri.normal * tri.intensity;
        total_intensity += tri.intensity;
    }

    header.intensity = total_intensity;
    header.weighted_normal = SafeNormalize(weighted_normal_acc);

    float variance_acc = 0.f;
    if (total_intensity > 0.f) {
        for (uint32_t tri_idx : triangle_members) {
            const auto & tri = triangles[tri_idx];
            float one_minus_dot = 1.f - glm::dot(header.weighted_normal, tri.normal);
            variance_acc += tri.intensity * one_minus_dot * one_minus_dot;
        }
        header.weighted_normal_variance = variance_acc / total_intensity;
    }

    return header;
}

MeshLightClusterHeader BuildClusterHeaderFromRuntimeNode(
    const std::vector<MeshLightTriangle> & triangles,
    const BuildRuntimeNode & node,
    uint32_t level,
    uint32_t triangle_offset
) {
    return BuildClusterHeaderFromTriangleMembers(triangles, node.triangle_indices, level, triangle_offset);
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
        config.weight_aabb_expand * pos_expand +
        config.weight_normal_aabb_expand * normal_expand +
        config.weight_non_topology_penalty * topology_penalty +
        config.weight_centroid_distance * centroid_dist +
        config.weight_normal_deviation * normal_deviation +
        config.weight_intensity_imbalance * intensity_imbalance;
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

MeshLightClusterLevel BuildSnapshotLevel(
    const std::vector<MeshLightTriangle> & triangles,
    const std::vector<MeshLightClusterNode> & nodes,
    const std::vector<uint32_t> & active_node_indices,
    uint32_t snapshot_level
) {
    MeshLightClusterLevel level {};
    level.level = snapshot_level;
    level.cluster_count = (uint32_t)active_node_indices.size();
    level.cluster_node_indices = active_node_indices;

    std::vector<uint32_t> sorted_node_indices = active_node_indices;
    std::sort(sorted_node_indices.begin(), sorted_node_indices.end(), [&](uint32_t a, uint32_t b) {
        return nodes[a].header.intensity > nodes[b].header.intensity;
    });

    level.clusters.reserve(sorted_node_indices.size());
    size_t total_tri_refs = 0;
    for (uint32_t node_idx : sorted_node_indices) {
        total_tri_refs += nodes[node_idx].triangle_indices.size();
    }
    level.triangle_indices.reserve(total_tri_refs);

    for (uint32_t node_idx : sorted_node_indices) {
        const auto & node = nodes[node_idx];
        uint32_t tri_offset = (uint32_t)level.triangle_indices.size();
        level.triangle_indices.insert(level.triangle_indices.end(), node.triangle_indices.begin(), node.triangle_indices.end());

        auto header = BuildClusterHeaderFromTriangleMembers(
            triangles,
            std::span<const uint32_t>(node.triangle_indices.data(), node.triangle_indices.size()),
            snapshot_level,
            tri_offset
        );
        level.clusters.push_back(header);
    }

    return level;
}

MergeEdge FindFallbackMergeEdge(
    const std::vector<BuildRuntimeNode> & runtime_nodes,
    const std::vector<uint32_t> & active_nodes,
    const MeshLightClusterBuildConfig & config,
    float inv_scene_diag
) {
    MergeEdge best {};
    for (uint32_t i = 0; i < (uint32_t)active_nodes.size(); ++i) {
        uint32_t a = active_nodes[i];
        if (!runtime_nodes[a].active) {
            continue;
        }
        for (uint32_t j = i + 1; j < (uint32_t)active_nodes.size(); ++j) {
            uint32_t b = active_nodes[j];
            if (!runtime_nodes[b].active) {
                continue;
            }
            float cost = EvaluatePairMergeCost(runtime_nodes[a], runtime_nodes[b], false, config, inv_scene_diag);
            if (cost < best.cost) {
                best = {a, b, cost, false};
            }
        }
    }
    return best;
}

} // namespace

MeshLightClusterHierarchy BuildMeshLightClusterHiearchy(
    const Geometry & geometry,
    const Material & material,
    const MeshLightClusterBuildConfig & config
) {
    MeshLightClusterHierarchy hierarchy {};
    hierarchy.source_triangle_count = geometry.GetIndexCount() / 3;

    if (geometry.GetIndexCount() < 3 || !material.IsEmissive()) {
        return hierarchy;
    }

    auto packed_material = material.PackMaterialHeader();
    float emissive_luma = SafeLuminance(packed_material.Emissive, material.GetEmissiveTexture() != nullptr);
    if (emissive_luma <= 0.f) {
        return hierarchy;
    }

    AABB total_aabb {};
    BuildTriangleList(
        geometry,
        material.GetEmissiveTexture(),
        emissive_luma,
        config,
        hierarchy.triangles,
        total_aabb,
        hierarchy.total_intensity
    );
    if (hierarchy.triangles.empty()) {
        return hierarchy;
    }

    const uint32_t leaf_count = (uint32_t)hierarchy.triangles.size();
    auto morton_order = BuildSortedTriangleIndicesByMorton(hierarchy.triangles, total_aabb);

    // Build topology and supplemented adjacency over filtered triangle indices.
    auto topology_adjacency = BuildTopologyAdjacency(geometry, hierarchy.triangles, hierarchy.source_triangle_count);
    auto augmented_adjacency = topology_adjacency;
    AugmentAdjacencyByMortonWindow(augmented_adjacency, morton_order, config.supplement_morton_neighbor_window);

    // Prepare leaf nodes.
    hierarchy.nodes.reserve(leaf_count * 2 - 1);
    std::vector<BuildRuntimeNode> runtime_nodes;
    runtime_nodes.reserve(leaf_count * 2 - 1);

    std::vector<uint32_t> active_nodes;
    active_nodes.reserve(leaf_count);

    for (uint32_t tri_idx = 0; tri_idx < leaf_count; ++tri_idx) {
        MeshLightClusterNode node {};
        node.is_leaf = true;
        node.triangle_indices = {tri_idx};
        node.header = BuildClusterHeaderFromTriangleMembers(
            hierarchy.triangles,
            std::span<const uint32_t>(node.triangle_indices.data(), node.triangle_indices.size()),
            0,
            0
        );

        BuildRuntimeNode rt {};
        rt.active = true;
        rt.position_aabb = hierarchy.triangles[tri_idx].local_aabb;
        rt.normal_aabb = {};
        rt.normal_aabb.Encapsulate(hierarchy.triangles[tri_idx].normal);
        rt.weighted_normal_sum = hierarchy.triangles[tri_idx].normal * hierarchy.triangles[tri_idx].intensity;
        rt.weighted_centroid_sum = hierarchy.triangles[tri_idx].centroid * hierarchy.triangles[tri_idx].intensity;
        rt.total_intensity = hierarchy.triangles[tri_idx].intensity;
        rt.triangle_indices = {tri_idx};
        rt.augmented_neighbors = augmented_adjacency[tri_idx];
        rt.topology_neighbors = topology_adjacency[tri_idx];

        uint32_t node_idx = (uint32_t)hierarchy.nodes.size();
        hierarchy.nodes.push_back(std::move(node));
        runtime_nodes.push_back(std::move(rt));
        active_nodes.push_back(node_idx);
    }

    // Merge priority queue initialized from augmented neighborhood pairs.
    std::priority_queue<MergeEdge, std::vector<MergeEdge>, MergeEdgeGreater> queue;

    AABB scene_aabb = total_aabb;
    float scene_diag = glm::length(glm::max(scene_aabb.max - scene_aabb.min, glm::vec3(1e-6f)));
    float inv_scene_diag = 1.f / std::max(scene_diag, 1e-6f);

    std::unordered_set<uint64_t> pushed_pairs;
    pushed_pairs.reserve(leaf_count * 8);

    auto try_push_pair = [&](uint32_t a, uint32_t b) {
        if (a == b || !runtime_nodes[a].active || !runtime_nodes[b].active) {
            return;
        }
        if (a > b) {
            std::swap(a, b);
        }
        uint64_t key = BuildEdgeKey(a, b);
        if (pushed_pairs.find(key) != pushed_pairs.end()) {
            return;
        }
        pushed_pairs.insert(key);

        bool topo_adj = runtime_nodes[a].topology_neighbors.find(b) != runtime_nodes[a].topology_neighbors.end();
        float cost = EvaluatePairMergeCost(runtime_nodes[a], runtime_nodes[b], topo_adj, config, inv_scene_diag);
        queue.push({a, b, cost, topo_adj});
    };

    for (uint32_t i = 0; i < leaf_count; ++i) {
        for (uint32_t n : runtime_nodes[i].augmented_neighbors) {
            try_push_pair(i, n);
        }
    }

    // Capture snapshots at N, N/2, N/4 ... 1 active clusters.
    auto snapshot_targets = BuildSnapshotTargets(leaf_count);
    uint32_t snapshot_cursor = 0;
    uint32_t active_count = leaf_count;

    hierarchy.levels.push_back(BuildSnapshotLevel(hierarchy.triangles, hierarchy.nodes, active_nodes, snapshot_cursor));
    snapshot_cursor = 1;

    while (active_count > 1) {
        MergeEdge best_edge {};
        bool found = false;

        while (!queue.empty()) {
            auto e = queue.top();
            queue.pop();
            if (!runtime_nodes[e.a].active || !runtime_nodes[e.b].active) {
                continue;
            }
            best_edge = e;
            found = true;
            break;
        }

        if (!found) {
            best_edge = FindFallbackMergeEdge(runtime_nodes, active_nodes, config, inv_scene_diag);
            found = best_edge.a != UINT32_MAX && best_edge.b != UINT32_MAX;
        }

        if (!found) {
            break;
        }

        uint32_t a = best_edge.a;
        uint32_t b = best_edge.b;

        // Build merged runtime node.
        BuildRuntimeNode merged_rt {};
        merged_rt.active = true;
        merged_rt.position_aabb = AABB::Merge(runtime_nodes[a].position_aabb, runtime_nodes[b].position_aabb);
        merged_rt.normal_aabb = AABB::Merge(runtime_nodes[a].normal_aabb, runtime_nodes[b].normal_aabb);
        merged_rt.weighted_normal_sum = runtime_nodes[a].weighted_normal_sum + runtime_nodes[b].weighted_normal_sum;
        merged_rt.weighted_centroid_sum = runtime_nodes[a].weighted_centroid_sum + runtime_nodes[b].weighted_centroid_sum;
        merged_rt.total_intensity = runtime_nodes[a].total_intensity + runtime_nodes[b].total_intensity;

        merged_rt.triangle_indices.reserve(runtime_nodes[a].triangle_indices.size() + runtime_nodes[b].triangle_indices.size());
        merged_rt.triangle_indices.insert(
            merged_rt.triangle_indices.end(),
            runtime_nodes[a].triangle_indices.begin(),
            runtime_nodes[a].triangle_indices.end()
        );
        merged_rt.triangle_indices.insert(
            merged_rt.triangle_indices.end(),
            runtime_nodes[b].triangle_indices.begin(),
            runtime_nodes[b].triangle_indices.end()
        );

        // Build merged node in tree.
        MeshLightClusterNode merged_node {};
        merged_node.is_leaf = false;
        merged_node.left_child = a;
        merged_node.right_child = b;
        merged_node.triangle_indices = merged_rt.triangle_indices;
        merged_node.header = BuildClusterHeaderFromRuntimeNode(hierarchy.triangles, merged_rt, 0, 0);

        uint32_t merged_idx = (uint32_t)hierarchy.nodes.size();
        hierarchy.nodes[a].parent = merged_idx;
        hierarchy.nodes[b].parent = merged_idx;

        hierarchy.nodes.push_back(std::move(merged_node));
        runtime_nodes.push_back(std::move(merged_rt));

        // Union neighbors and rewire.
        std::unordered_set<uint32_t> union_aug = runtime_nodes[a].augmented_neighbors;
        union_aug.insert(runtime_nodes[b].augmented_neighbors.begin(), runtime_nodes[b].augmented_neighbors.end());
        union_aug.erase(a);
        union_aug.erase(b);

        std::unordered_set<uint32_t> union_topo = runtime_nodes[a].topology_neighbors;
        union_topo.insert(runtime_nodes[b].topology_neighbors.begin(), runtime_nodes[b].topology_neighbors.end());
        union_topo.erase(a);
        union_topo.erase(b);

        for (uint32_t n : union_aug) {
            if (!runtime_nodes[n].active) {
                continue;
            }
            runtime_nodes[n].augmented_neighbors.erase(a);
            runtime_nodes[n].augmented_neighbors.erase(b);
            runtime_nodes[n].augmented_neighbors.insert(merged_idx);

            bool topo_link =
                runtime_nodes[n].topology_neighbors.find(a) != runtime_nodes[n].topology_neighbors.end() ||
                runtime_nodes[n].topology_neighbors.find(b) != runtime_nodes[n].topology_neighbors.end();
            runtime_nodes[n].topology_neighbors.erase(a);
            runtime_nodes[n].topology_neighbors.erase(b);
            if (topo_link) {
                runtime_nodes[n].topology_neighbors.insert(merged_idx);
            }

            runtime_nodes[merged_idx].augmented_neighbors.insert(n);
            if (topo_link || union_topo.find(n) != union_topo.end()) {
                runtime_nodes[merged_idx].topology_neighbors.insert(n);
            }

            try_push_pair(merged_idx, n);
        }

        runtime_nodes[a].active = false;
        runtime_nodes[b].active = false;
        runtime_nodes[a].augmented_neighbors.clear();
        runtime_nodes[b].augmented_neighbors.clear();
        runtime_nodes[a].topology_neighbors.clear();
        runtime_nodes[b].topology_neighbors.clear();

        active_count -= 1;
        active_nodes.push_back(merged_idx);

        // Capture snapshot when crossing target count exactly.
        while (snapshot_cursor < snapshot_targets.size() && active_count == snapshot_targets[snapshot_cursor]) {
            std::vector<uint32_t> alive;
            alive.reserve(active_count);
            for (uint32_t node_idx : active_nodes) {
                if (node_idx < runtime_nodes.size() && runtime_nodes[node_idx].active) {
                    alive.push_back(node_idx);
                }
            }
            hierarchy.levels.push_back(BuildSnapshotLevel(hierarchy.triangles, hierarchy.nodes, alive, snapshot_cursor));
            ++snapshot_cursor;
        }
    }

    // Determine root.
    for (int i = (int)runtime_nodes.size() - 1; i >= 0; --i) {
        if (runtime_nodes[i].active) {
            hierarchy.root_node_index = (uint32_t)i;
            break;
        }
    }

    // Ensure final level exists (in case of disconnected/fallback edge corner cases).
    if (hierarchy.root_node_index != UINT32_MAX && (hierarchy.levels.empty() || hierarchy.levels.back().cluster_count != 1)) {
        hierarchy.levels.push_back(BuildSnapshotLevel(
            hierarchy.triangles,
            hierarchy.nodes,
            std::vector<uint32_t>{hierarchy.root_node_index},
            (uint32_t)hierarchy.levels.size()
        ));
    }

    return hierarchy;
}

MeshLightClusterSelection SelectMeshLightPrimitivesByBudget(
    const MeshLightClusterHierarchy & hierarchy,
    uint32_t max_lights,
    bool prefer_brightest_triangle_in_cluster
) {
    MeshLightClusterSelection result {};
    if (hierarchy.Empty() || max_lights == 0) {
        return result;
    }

    uint32_t selected_level = (uint32_t)hierarchy.levels.size() - 1;
    uint32_t best_cluster_count = 0;
    for (uint32_t level = 0; level < (uint32_t)hierarchy.levels.size(); ++level) {
        uint32_t num_clusters = (uint32_t)hierarchy.levels[level].clusters.size();
        if (num_clusters <= max_lights && num_clusters > best_cluster_count) {
            selected_level = level;
            best_cluster_count = num_clusters;
        }
    }

    const auto & level = hierarchy.levels[selected_level];
    result.selected_level = selected_level;
    result.selected_cluster_count = (uint32_t)level.clusters.size();
    result.primitive_indices.reserve(level.clusters.size());

    for (const auto & cluster : level.clusters) {
        if (cluster.triangle_count == 0) {
            continue;
        }

        uint32_t picked_triangle_index = level.triangle_indices[cluster.triangle_offset];
        if (prefer_brightest_triangle_in_cluster && cluster.triangle_count > 1) {
            float max_intensity = -1.f;
            for (uint32_t i = 0; i < cluster.triangle_count; ++i) {
                uint32_t tri_idx = level.triangle_indices[cluster.triangle_offset + i];
                float tri_intensity = hierarchy.triangles[tri_idx].intensity;
                if (tri_intensity > max_intensity) {
                    max_intensity = tri_intensity;
                    picked_triangle_index = tri_idx;
                }
            }
        }

        result.primitive_indices.push_back(hierarchy.triangles[picked_triangle_index].primitive_index);
    }

    return result;
}

MI_NAMESPACE_END
