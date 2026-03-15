/*
 * Created: 2026/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <chrono>
#include <iostream>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include <renderer/mi_geometry.h>
#include <renderer/mi_material.h>
#include <renderer/r_light_cluster_hiearchy.h>

MI_NAMESPACE_BEGIN

namespace {

TRef<Geometry> CreateUVSphereGeometry(uint32_t num_lat, uint32_t num_lon, float radius = 1.0f) {
    std::vector<DefaultStaticMeshVertex> vertices;
    std::vector<uint32_t> indices;

    vertices.reserve((num_lat + 1) * (num_lon + 1));
    for (uint32_t lat = 0; lat <= num_lat; ++lat) {
        float v = float(lat) / float(num_lat);
        float theta = v * glm::pi<float>();
        float sin_theta = std::sin(theta);
        float cos_theta = std::cos(theta);

        for (uint32_t lon = 0; lon <= num_lon; ++lon) {
            float u = float(lon) / float(num_lon);
            float phi = u * glm::two_pi<float>();
            float sin_phi = std::sin(phi);
            float cos_phi = std::cos(phi);

            glm::vec3 n = glm::normalize(glm::vec3(cos_phi * sin_theta, cos_theta, sin_phi * sin_theta));
            DefaultStaticMeshVertex vert {};
            vert.Position = n * radius;
            vert.Normal = n;
            vert.UV = glm::vec2(u, v);
            vertices.push_back(vert);
        }
    }

    for (uint32_t lat = 0; lat < num_lat; ++lat) {
        for (uint32_t lon = 0; lon < num_lon; ++lon) {
            uint32_t row0 = lat * (num_lon + 1);
            uint32_t row1 = (lat + 1) * (num_lon + 1);
            uint32_t i0 = row0 + lon;
            uint32_t i1 = row0 + lon + 1;
            uint32_t i2 = row1 + lon;
            uint32_t i3 = row1 + lon + 1;

            if (lat != 0) {
                indices.push_back(i0);
                indices.push_back(i2);
                indices.push_back(i1);
            }
            if (lat + 1 != num_lat) {
                indices.push_back(i1);
                indices.push_back(i2);
                indices.push_back(i3);
            }
        }
    }

    return Geometry::CreateFromVertices(vertices, indices);
}

TRef<Material> CreateEmissiveMaterial() {
    auto mat = Material::Create("test_emissive", glm::vec4(1.f), 0.5f, glm::vec3(4.f, 3.f, 2.f));
    mat->SetOpaque(true);
    return mat;
}

bool IsValidClusterChild(const MeshLightClusterHierarchy & hierarchy, const MeshLightClusterChild & child) {
    if (child.bIsLeaf) {
        return child.Index < hierarchy.triangles.size();
    }
    return child.Index < hierarchy.nodes.size();
}

uint32_t SelectLevelIndexByBudget(const MeshLightClusterHierarchy & hierarchy, uint32_t budget) {
    if (hierarchy.levels.empty()) {
        return 0;
    }

    uint32_t selected_level = (uint32_t)hierarchy.levels.size() - 1;
    for (uint32_t i = 0; i < hierarchy.levels.size(); ++i) {
        if (hierarchy.levels[i].level_node_indices.size() <= budget) {
            selected_level = i;
            break;
        }
    }
    return selected_level;
}

} // namespace

TEST(RendererLightClusterHierarchyTest, BuildHierarchyBasicStructure) {
    auto geom = CreateUVSphereGeometry(12, 16, 1.0f);
    auto mat = CreateEmissiveMaterial();

    MeshLightClusterBuildConfig config {};
    config.supplement_morton_neighbor_window = 3;

    auto hierarchy_opt = BuildMeshLightClusterHierarchy(*geom, *mat, config);
    ASSERT_TRUE(hierarchy_opt.has_value());
    auto hierarchy = hierarchy_opt.value_or(MeshLightClusterHierarchy{});

    ASSERT_FALSE(hierarchy.Empty());
    ASSERT_FALSE(hierarchy.triangles.empty());
    ASSERT_GE(hierarchy.levels.size(), 2u);
    ASSERT_EQ(hierarchy.headers.size(), hierarchy.nodes.size());
    EXPECT_GT(hierarchy.total_intensity, 0.f);
    EXPECT_TRUE(IsValidClusterChild(hierarchy, hierarchy.root_node));

    if (hierarchy.IsSingleTriangle()) {
        EXPECT_TRUE(hierarchy.root_node.bIsLeaf);
    } else {
        EXPECT_FALSE(hierarchy.root_node.bIsLeaf);
    }

    size_t prev_level_size = hierarchy.levels.front().level_node_indices.size();
    for (const auto & level : hierarchy.levels) {
        ASSERT_FALSE(level.level_node_indices.empty());
        EXPECT_LE(level.level_node_indices.size(), prev_level_size);
        prev_level_size = level.level_node_indices.size();

        std::unordered_set<uint64_t> unique_nodes;
        unique_nodes.reserve(level.level_node_indices.size());

        for (const auto & child : level.level_node_indices) {
            EXPECT_TRUE(IsValidClusterChild(hierarchy, child));
            uint64_t key = (uint64_t(child.bIsLeaf) << 32u) | uint64_t(child.Index);
            unique_nodes.insert(key);
        }
        EXPECT_EQ(unique_nodes.size(), level.level_node_indices.size());
    }

    EXPECT_EQ(hierarchy.levels.front().level_node_indices.size(), hierarchy.triangles.size());
    EXPECT_EQ(hierarchy.levels.back().level_node_indices.size(), 1u);
}

TEST(RendererLightClusterHierarchyTest, SnapshotLevelsSupportBudgetDrivenSelection) {
    auto geom = CreateUVSphereGeometry(20, 30, 1.0f);
    auto mat = CreateEmissiveMaterial();

    MeshLightClusterBuildConfig config {};
    auto hierarchy_opt = BuildMeshLightClusterHierarchy(*geom, *mat, config);
    ASSERT_TRUE(hierarchy_opt.has_value());
    auto hierarchy = hierarchy_opt.value_or(MeshLightClusterHierarchy{});
    ASSERT_FALSE(hierarchy.Empty());

    const std::vector<uint32_t> budgets {1u, 4u, 16u, 64u, 256u};
    for (uint32_t budget : budgets) {
        uint32_t selected_level = SelectLevelIndexByBudget(hierarchy, budget);
        ASSERT_LT(selected_level, hierarchy.levels.size());
        const auto & selected_nodes = hierarchy.levels[selected_level].level_node_indices;

        EXPECT_GT(selected_nodes.size(), 0u);
        EXPECT_LE(selected_nodes.size(), std::max(1u, budget));

        std::unordered_set<uint64_t> unique_nodes;
        unique_nodes.reserve(selected_nodes.size());
        for (const auto & child : selected_nodes) {
            EXPECT_TRUE(IsValidClusterChild(hierarchy, child));
            uint64_t key = (uint64_t(child.bIsLeaf) << 32u) | uint64_t(child.Index);
            unique_nodes.insert(key);
        }
        EXPECT_EQ(unique_nodes.size(), selected_nodes.size());
    }
}

TEST(RendererLightClusterHierarchyTest, PerformanceSmokeBuildHierarchy) {
    auto geom = CreateUVSphereGeometry(64, 96, 1.0f);
    auto mat = CreateEmissiveMaterial();

    MeshLightClusterBuildConfig config {};
    config.supplement_morton_neighbor_window = 2;

    auto t0 = std::chrono::high_resolution_clock::now();
    auto hierarchy_opt = BuildMeshLightClusterHierarchy(*geom, *mat, config);
    ASSERT_TRUE(hierarchy_opt.has_value());
    auto hierarchy = hierarchy_opt.value_or(MeshLightClusterHierarchy{});
    auto t1 = std::chrono::high_resolution_clock::now();

    ASSERT_FALSE(hierarchy.Empty());
    ASSERT_FALSE(hierarchy.levels.back().level_node_indices.empty());

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout
        << "[LightClusterTest] triangles=" << hierarchy.triangles.size()
        << ", levels=" << hierarchy.levels.size()
        << ", coarsest_level_nodes=" << hierarchy.levels.back().level_node_indices.size()
        << ", elapsed_ms=" << elapsed_ms
        << std::endl;

    // Smoke threshold to catch pathological regressions while keeping test robust on debug builds.
    EXPECT_LT(elapsed_ms, 10000.0);
}

MI_NAMESPACE_END

int main(int argc, char ** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
