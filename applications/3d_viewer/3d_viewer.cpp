/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "viewer_app.h"

using namespace MI_NAMESPACE;

/*
    // Load default model
    enum DEFAULT_MODEL_TYPE {
        MESH_ONLY,
        MESH_AND_VOLUME_PRIMITIVES,
        MESH_AND_VOLUME_GRID
    };

    DEFAULT_MODEL_TYPE default_model_type = MESH_AND_VOLUME_GRID;

    switch(default_model_type) {
        case MESH_ONLY : {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            // auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room/scene.gltf");
            // auto model_path = std::filesystem::path("D:/TestScene/remi-room/RemiIndoorsHard.gltf");
            auto model_path = std::filesystem::path("D:/TestScene/room/Room.gltf");
            // auto model_path = std::filesystem::path("D:/TestScene/BugTest/Bug.gltf");
            if (!GLTFLoader::LoadGLTF(
                model_path,
                *resource_allocator,
                *scene, default_mat.Raw(),
                geometries, materials, meshes
            )) {
                MI_WARN("Failed to load GLTF model {}.", model_path.string());
            } else {
            }
            auto & r = Renderer::Get();
            for (auto e : meshes) {
                e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
            }
            break;
        }
        case MESH_AND_VOLUME_PRIMITIVES : {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room_empty/scene.gltf");
            if (!GLTFLoader::LoadGLTF(
                model_path,
                *resource_allocator,
                *scene, default_mat.Raw(),
                geometries, materials, meshes
            )) {
                MI_WARN("Failed to load GLTF model {}.", model_path.string());
            } else {
            }
            auto & r = Renderer::Get();
            for (auto e : meshes) {
                e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
            }
            TRef<VolumePrimitives> volprims;
            VolumePrimitivesLoader::LoadPLY(
                GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/puppy/point_cloud.ply"),
                // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/grid/point_cloud.ply"),
                // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/simple_volume/point_cloud_1point.ply"),
                // GetInfra().TranslateResPathToFilePath("C:/Users/hineven/CLionProjects/3DGS_GI/data/armadillo/point_cloud/iteration_35000/point_cloud.ply"),
                // GetInfra().TranslateResPathToFilePath("C:/Users/hineven/CLionProjects/3DGS_GI/data/barn/point_cloud/iteration_50000/point_cloud.ply"),
                *resource_allocator, volprims//, 0.1f
            );
            if (volprims) {
                volprims->UpdateOnDevice(resource_allocator.Raw());
                auto volprims_instance = VolumePrimitivesInstance::Create(scene.get(), volprims.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
                volprims_instance->EditTransform().Translate({0, 0.5, 0});
                // volprims_instance->EditTransform().Scale({0.1f, 0.1f, 0.1f});
            }
            break;
        }
        case MESH_AND_VOLUME_GRID : {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room_empty/scene.gltf");
            if (!GLTFLoader::LoadGLTF(
                model_path,
                *resource_allocator,
                *scene, default_mat.Raw(),
                geometries, materials, meshes
            )) {
                MI_WARN("Failed to load GLTF model {}.", model_path.string());
            } else {
            }
            auto & r = Renderer::Get();
            for (auto e : meshes) {
                e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
            }
            TRef<VolumeGrid> volume_grid;
            volume_grid = OpenVDBLoader::LoadVDB(
                // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/bunny/bunny_density.vdb"),
                // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/bunny/bunny_density_color.vdb"),
                // GetInfra().TranslateResPathToFilePath("D:/Coding/Houdini/Houdini_Works/bunny_density.vdb"),
                GetInfra().TranslateResPathToFilePath("D:/Coding/Houdini/Houdini_Works/bunny_density_color.vdb"),
                *resource_allocator, {1024, "density", "color"}
            );
            if(volume_grid) {
                volume_grid->UpdateOnDevice(resource_allocator.Raw());
                auto volume_grid_instance = VolumeGridInstance::Create(scene.get(), volume_grid.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
            }
            break;
        }
        default : break;
    }
    */

int main () {
    MainLoopStartConfig cfg {};
    cfg.window_width = 1920;
    cfg.window_height = 1080;

#if MI_ENABLE_SHADER_DEBUGGING
    auto infra = std::make_unique<MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<MyInfra>(true);
#endif
    Run3DViewer(std::move(infra), cfg);
    return 0;
}

