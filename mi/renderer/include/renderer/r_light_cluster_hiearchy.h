/*
 * Created: 2026/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_LIGHT_CLUSTER_HIEARCHY_PUBLIC_H
#define MI_R_LIGHT_CLUSTER_HIEARCHY_PUBLIC_H

#include <vector>

#include <renderer/mi_aabb.h>
#include <renderer/mi_geometry.h>
#include <renderer/mi_material.h>

MI_NAMESPACE_BEGIN

// Mesh light hierarchy build configuration.
//
// Design goals:
// 1) Build a tree hierarchy over emissive triangles with topology-aware greedy merging.
// 2) Capture independent LOD snapshots when number of active clusters reaches N, N/2, N/4 ... 1.
// 3) Preserve per-cluster statistics needed by runtime importance sampling and injection.
struct MeshLightClusterBuildConfig {
    float min_triangle_area {1e-10f};
    float min_cluster_intensity {1e-8f};

    // Build extra non-topological neighborhood links from Morton ordering window.
    // These links provide candidates for merging disconnected but spatially close pieces.
    uint32_t supplement_morton_neighbor_window {3};

    // Greedy pair-merge heuristic weights.
    // Total cost = weighted sum of the terms below.
    float weight_aabb_expand {1.0f};
    float weight_normal_aabb_expand {1.2f};
    float weight_non_topology_penalty {0.75f};
    float weight_centroid_distance {0.35f};
    float weight_normal_deviation {1.f};
    float weight_intensity_imbalance {0.25f};

    // Penalty value used when merging non-topology-adjacent clusters.
    float non_topology_penalty_value {1.0f};
};

struct MeshLightTriangle {
    uint32_t primitive_index {UINT32_MAX};
    glm::vec3 v0 {};
    glm::vec3 v1 {};
    glm::vec3 v2 {};
    glm::vec3 normal {0.f, 0.f, 1.f};
    glm::vec3 centroid {};
    AABB local_aabb {};
    float area {};
    float intensity {};
};

struct MeshLightClusterHeader {
    AABB local_aabb {};
    glm::vec3 weighted_normal {0.f, 0.f, 1.f};
    float weighted_normal_variance {};
    float intensity {};
    uint32_t level {};
    uint32_t triangle_offset {};
    uint32_t triangle_count {};
};

// Tree node representing one merged emissive cluster.
// Leaf node: left_child/right_child are UINT32_MAX and triangle_indices contains one triangle.
// Internal node: children point to previous nodes and triangle_indices stores merged leaves.
struct MeshLightClusterNode {
    uint32_t parent {UINT32_MAX};
    uint32_t left_child {UINT32_MAX};
    uint32_t right_child {UINT32_MAX};
    bool is_leaf {false};

    MeshLightClusterHeader header {};
    std::vector<uint32_t> triangle_indices;
};

struct MeshLightClusterLevel {
    // Snapshot level index in ascending coarseness order:
    // level 0 is N clusters, level 1 is around N/2, ... last level is 1.
    uint32_t level {};
    uint32_t cluster_count {};

    // Node indices in hierarchy.nodes used by this snapshot.
    std::vector<uint32_t> cluster_node_indices;

    // Compact per-level arrays kept for easy GPU upload and backward compatibility.
    std::vector<MeshLightClusterHeader> clusters;
    std::vector<uint32_t> triangle_indices;
};

struct MeshLightClusterHierarchy {
    uint32_t source_triangle_count {};
    float total_intensity {};
    std::vector<MeshLightTriangle> triangles;

    // Full merge tree and root node index.
    std::vector<MeshLightClusterNode> nodes;
    uint32_t root_node_index {UINT32_MAX};

    // Snapshot LOD levels captured at N, N/2, N/4 ... 1 active clusters.
    std::vector<MeshLightClusterLevel> levels;

    FORCEINLINE bool Empty() const {
        return triangles.empty() || levels.empty();
    }
};

struct MeshLightClusterSelection {
    uint32_t selected_level {};
    uint32_t selected_cluster_count {};
    std::vector<uint32_t> primitive_indices;
};

MeshLightClusterHierarchy BuildMeshLightClusterHiearchy(
    const Geometry & geometry,
    const Material & material,
    const MeshLightClusterBuildConfig & config = {}
);

MeshLightClusterSelection SelectMeshLightPrimitivesByBudget(
    const MeshLightClusterHierarchy & hierarchy,
    uint32_t max_lights,
    bool prefer_brightest_triangle_in_cluster = true
);

MI_NAMESPACE_END

#endif // MI_R_LIGHT_CLUSTER_HIEARCHY_PUBLIC_H
