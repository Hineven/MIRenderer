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

} // namespace

TEST(RendererLightClusterHierarchyTest, BuildHierarchyBasicStructure) {
    auto geom = CreateUVSphereGeometry(12, 16, 1.0f);
    auto mat = CreateEmissiveMaterial();

    MeshLightClusterBuildConfig config {};
    config.supplement_morton_neighbor_window = 3;

    auto hierarchy = BuildMeshLightClusterHiearchy(*geom, *mat, config);

    ASSERT_FALSE(hierarchy.Empty());
    ASSERT_FALSE(hierarchy.triangles.empty());
    ASSERT_GE(hierarchy.levels.size(), 2u);
    EXPECT_GT(hierarchy.total_intensity, 0.f);

    for (const auto & level : hierarchy.levels) {
        EXPECT_EQ(level.triangle_indices.size(), hierarchy.triangles.size());
        ASSERT_FALSE(level.clusters.empty());
        for (const auto & cluster : level.clusters) {
            EXPECT_GT(cluster.triangle_count, 0u);
            EXPECT_GT(cluster.intensity, 0.f);
            EXPECT_TRUE(cluster.local_aabb.IsValid());
            EXPECT_LT(cluster.triangle_offset + cluster.triangle_count, level.triangle_indices.size() + 1);
        }
    }

    EXPECT_EQ(hierarchy.levels.back().clusters.size(), 1u);
}

TEST(RendererLightClusterHierarchyTest, BudgetSelectionRespectsMaxLights) {
    auto geom = CreateUVSphereGeometry(20, 30, 1.0f);
    auto mat = CreateEmissiveMaterial();

    MeshLightClusterBuildConfig config {};
    auto hierarchy = BuildMeshLightClusterHiearchy(*geom, *mat, config);
    ASSERT_FALSE(hierarchy.Empty());

    constexpr uint32_t kBudget = 64;
    auto selection = SelectMeshLightPrimitivesByBudget(hierarchy, kBudget, true);

    EXPECT_LE(selection.primitive_indices.size(), kBudget);
    EXPECT_GT(selection.primitive_indices.size(), 0u);
    EXPECT_LT(selection.selected_level, hierarchy.levels.size());

    std::unordered_set<uint32_t> unique_indices;
    unique_indices.reserve(selection.primitive_indices.size());
    for (uint32_t primitive_idx : selection.primitive_indices) {
        EXPECT_LT(primitive_idx, hierarchy.source_triangle_count);
        unique_indices.insert(primitive_idx);
    }
    EXPECT_EQ(unique_indices.size(), selection.primitive_indices.size());
}

TEST(RendererLightClusterHierarchyTest, PerformanceSmokeBuildAndSelect) {
    auto geom = CreateUVSphereGeometry(64, 96, 1.0f);
    auto mat = CreateEmissiveMaterial();

    MeshLightClusterBuildConfig config {};
    config.supplement_morton_neighbor_window = 2;

    auto t0 = std::chrono::high_resolution_clock::now();
    auto hierarchy = BuildMeshLightClusterHiearchy(*geom, *mat, config);
    auto selection = SelectMeshLightPrimitivesByBudget(hierarchy, 128, true);
    auto t1 = std::chrono::high_resolution_clock::now();

    ASSERT_FALSE(hierarchy.Empty());
    ASSERT_FALSE(selection.primitive_indices.empty());

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout
        << "[LightClusterTest] triangles=" << hierarchy.triangles.size()
        << ", levels=" << hierarchy.levels.size()
        << ", selected_level=" << selection.selected_level
        << ", selected_lights=" << selection.primitive_indices.size()
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
