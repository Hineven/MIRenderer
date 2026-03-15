/*
 * Created: 2026/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include <core/infra.h>
#include <core/task.h>
#include <infra_impl/infra.h>
#include <renderer/mi_material.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_scene.h>
#include <renderer/r_light_cluster_hiearchy.h>
#include <renderer/mi_static_mesh.h>
#include <rhi/rhi.h>
#include <util/gltf_loader.h>
#include <util/renderable_node.h>

MI_NAMESPACE_BEGIN

namespace {

class GltfClusterTestRuntime {
public:
    GltfClusterTestRuntime() {
        TransferInfra(std::make_unique<MyInfra>(true));
        GetInfra().Init();

        SetCurrentThreadType(ThreadType::kRenderThread);

        RHI::InitializeSingleton(RHIType::kVulkan);
        TaskGraph::InitializeSingleton(2, 2);
    }

    ~GltfClusterTestRuntime() {
        TaskGraph::DestroySingleton();
        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    }
};

std::filesystem::path ResolveGltfAssetPathForTemplateTest() {
    if (const char * env = std::getenv("MI_TEST_GLTF_PATH"); env && env[0] != '\0') {
        return std::filesystem::path(env);
    }

    // Preferred source-tree template location.
    std::filesystem::path source_default = std::filesystem::path("tests") / "renderer" / "resources" / "cluster_test_scene.gltf";
    if (std::filesystem::exists(source_default)) {
        return source_default;
    }

    // Build-tree copied resource location.
    std::filesystem::path build_default = std::filesystem::path("renderer") / "resources" / "cluster_test_scene.gltf";
    if (std::filesystem::exists(build_default)) {
        return build_default;
    }

    return {};
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

TEST(RendererLightClusterHierarchyGltfTemplateTest, BuildFromGltfAssetTemplate) {
    auto gltf_path = ResolveGltfAssetPathForTemplateTest();
    if (gltf_path.empty() || !std::filesystem::exists(gltf_path)) {
        GTEST_SKIP()
            << "No glTF asset found for template test. Put asset at tests/renderer/resources/cluster_test_scene.gltf "
            << "or set MI_TEST_GLTF_PATH.";
    }

    GltfClusterTestRuntime runtime;

    DeviceBindlessResourceAllocator allocator;
    Scene scene;
    scene.CreateOnDevice();

    auto node_registry = Create<RenderableNodeRegistry>();
    auto default_material = Material::Create("gltf_default", glm::vec4(1.f), 0.5f, glm::vec3(0.f));

    std::vector<TRef<Geometry>> geometries;
    std::vector<TRef<Material>> materials;
    std::vector<TRef<StaticMeshInstance>> meshes;

    bool loaded = GLTFLoader::LoadGLTF(
        gltf_path,
        allocator,
        scene,
        node_registry.Raw(),
        default_material.Raw(),
        geometries,
        materials,
        meshes,
        nullptr
    );

    ASSERT_TRUE(loaded) << "Failed to load glTF asset: " << gltf_path.string();
    ASSERT_FALSE(geometries.empty()) << "No geometry loaded from glTF asset: " << gltf_path.string();

    MeshLightClusterBuildConfig config {};
    config.supplement_morton_neighbor_window = 3;

    constexpr uint32_t kBudget = 128;
    uint32_t num_emissive_geometries = 0;
    uint32_t total_selected_nodes = 0;
    uint32_t max_selected_level = 0;

    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < geometries.size() && i < materials.size(); ++i) {
        if (!materials[i] || !materials[i]->IsEmissive()) {
            continue;
        }

        auto hierarchy_opt = BuildMeshLightClusterHierarchy(*geometries[i], *materials[i], config);
        auto hierarchy = hierarchy_opt.has_value() ? std::move(hierarchy_opt.value()) : MeshLightClusterHierarchy{};
        if (hierarchy.Empty()) {
            continue;
        }

        uint32_t selected_level = SelectLevelIndexByBudget(hierarchy, kBudget);
        ASSERT_LT(selected_level, hierarchy.levels.size());
        const auto & selected_nodes = hierarchy.levels[selected_level].level_node_indices;
        EXPECT_LE(selected_nodes.size(), std::max(1u, kBudget));
        EXPECT_FALSE(selected_nodes.empty());

        std::unordered_set<uint64_t> unique_nodes;
        unique_nodes.reserve(selected_nodes.size());
        for (const auto & child : selected_nodes) {
            uint64_t key = (uint64_t(child.bIsLeaf) << 32u) | uint64_t(child.Index);
            unique_nodes.insert(key);
        }
        EXPECT_EQ(unique_nodes.size(), selected_nodes.size());

        ++num_emissive_geometries;
        total_selected_nodes += (uint32_t)selected_nodes.size();
        max_selected_level = std::max(max_selected_level, selected_level);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    if (num_emissive_geometries == 0) {
        GTEST_SKIP() << "Asset loaded but no emissive geometries found. Set emissive material(s) in the glTF asset.";
    }

    double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout
        << "[LightClusterGLTFTemplate] asset=" << gltf_path.string()
        << ", emissive_geometries=" << num_emissive_geometries
        << ", total_selected_nodes=" << total_selected_nodes
        << ", max_selected_level=" << max_selected_level
        << ", elapsed_ms=" << elapsed_ms
        << std::endl;

    EXPECT_LT(elapsed_ms, 60000.0);
}

MI_NAMESPACE_END
