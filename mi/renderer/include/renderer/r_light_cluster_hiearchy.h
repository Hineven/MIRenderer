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
#include "../../shaders/shared/SharedLightClusterHierarchy.hlsl"

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

struct EvaluatedMeshLightTriangle {
    float area {};
    float intensity {};
    glm::vec3 v0 {};
    glm::vec3 v1 {};
    glm::vec3 v2 {};
};

struct MeshLightClusterLevel {
    std::vector<MeshLightClusterChild> level_node_indices; // Index into MeshLightClusterHierarchy.nodes
};

struct MeshLightClusterHierarchy {
    float total_intensity {};
    MeshLightClusterChild root_node {true, UINT32_MAX}; // Root node reference. Can be a leaf triangle if the hierarchy degenerates to a single triangle.

    //       R <- root node (cluster, indexed within `headers` and `nodes`)
    //      / \
    //    A    B  <- internal node (cluster, indexed within `headers` and `nodes`)
    //   / \  / \
    //  C  D L1 L2 <- leaf node (triangle, indexed within `triangles`)
    // ... ...

    // Every node in the binary tree must have zero or two children.

    // Leaf triangle light data.
    std::vector<MeshLightTriangle> triangles;
    // Headers for clusters (internal tree nodes).
    std::vector<MeshLightClusterHeader> headers;
    // Nodes for clusters (internal tree nodes).
    std::vector<MeshLightClusterNode> nodes;

    // LOD levels captured at N, N/2, N/4 ... 1 active clusters.
    std::vector<MeshLightClusterLevel> levels;

    FORCEINLINE bool Empty() const {
        return triangles.empty() || levels.empty();
    }

    FORCEINLINE bool IsSingleTriangle () const {
        return triangles.size() == 1;
    }
};

std::optional<MeshLightClusterHierarchy> BuildMeshLightClusterHierarchy(
    const Geometry & geometry,
    const Material & material,
    const MeshLightClusterBuildConfig & config = {}
);

MI_NAMESPACE_END

#endif // MI_R_LIGHT_CLUSTER_HIEARCHY_PUBLIC_H
