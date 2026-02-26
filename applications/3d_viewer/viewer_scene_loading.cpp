/*
 * Created: 2026/1/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "viewer_app.h"
#include "renderer/mi_texture.h"
#include "util/gaussian_radiance_field_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"
#include "util/texture_loader.h"
#include "util/openvdb_loader.h"
#include "util/renderable_node.h"

MI_NAMESPACE_BEGIN
void ViewerApp::LoadScene(const MainLoopStartConfig& cfg) {

    scene_ = std::make_unique<Scene>();
    
    {
        auto sky_file = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/tief_etz_4k.exr");
        sky_cube_ = TextureLoader::LoadEnvironmentMap("Sky", sky_file);
        if (sky_cube_) {
            sky_cube_->UpdateOnDevice();
            sky_cube_->ConvertToBindless();
            if (!sky_cube_->IsBindless() || !sky_cube_->GetDeviceTexture()) {
                MI_WARN("Sky cubemap '{}' failed to become a valid bindless device texture (bindless={}, device_tex={}). Environment light sampling may be invalid.",
                    sky_file.string(),
                    sky_cube_->IsBindless() ? 1 : 0,
                    sky_cube_->GetDeviceTexture() ? 1 : 0);
            }
        } else {
            MI_WARN("Failed to load sky environment map from '{}'.", sky_file.string());
        }
        scene_->SetSkyCube(sky_cube_.Raw());
    }

    default_material_ = Material::Create("default_material_", {0.8f, 0.8f, 0.8f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});

    {
        auto arrow_mat_x = Material::Create("arrow_mat_x", {1.0f, 0.0f, 0.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        auto arrow_mat_y = Material::Create("arrow_mat_y", {0.0f, 1.0f, 0.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        auto arrow_mat_z = Material::Create("arrow_mat_z", {0.0f, 0.0f, 1.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        arrow_mat_x->SetForward(true);
        arrow_mat_y->SetForward(true);
        arrow_mat_z->SetForward(true);
        auto& r = Renderer::Get();
        arrow_mat_x->UpdateOnDevice(r.GetDeviceAllocator());
        arrow_mat_y->UpdateOnDevice(r.GetDeviceAllocator());
        arrow_mat_z->UpdateOnDevice(r.GetDeviceAllocator());
        TRef<StaticMeshInstance> original_arrow_instance;
        {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            std::vector<TRef<StaticMeshInstance>> meshes;
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/internal/arrow.gltf");
            if (!GLTFLoader::LoadGLTF(
                model_path,
                *resource_allocator_,
                *scene_,
                renderable_node_registry_.Raw(),
                default_material_.Raw(),
                geometries, materials, meshes
            )) {
                MI_WARN("Failed to load GLTF model {}.", model_path.string());
            }
            original_arrow_instance = meshes.back();
            auto mesh = original_arrow_instance->GetStaticMesh();
            arrow_geometry_ = mesh->GetGeometries()[0];
        }
        {
            float scale = 0.25f;
            arrow_mesh_x_ = StaticMesh::Create(false, false);
            arrow_mesh_x_->AddMeshPrimitive(arrow_geometry_.Raw(), arrow_mat_x.Raw());
            arrow_mesh_x_->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_x_instance_ = StaticMeshInstance::Create(scene_.get(), arrow_mesh_x_.Raw(),
                original_arrow_instance->GetTransform().Scaled(glm::vec3(scale)));

            arrow_mesh_y_ = StaticMesh::Create(false, false);
            arrow_mesh_y_->AddMeshPrimitive(arrow_geometry_.Raw(), arrow_mat_y.Raw());
            arrow_mesh_y_->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_y_instance_ = StaticMeshInstance::Create(scene_.get(), arrow_mesh_y_.Raw(),
                original_arrow_instance->GetTransform().RotatedAbout(glm::radians(90.0f), {0, 0, 1}).Scaled(glm::vec3(scale)));

            arrow_mesh_z_ = StaticMesh::Create(false, false);
            arrow_mesh_z_->AddMeshPrimitive(arrow_geometry_.Raw(), arrow_mat_z.Raw());
            arrow_mesh_z_->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_z_instance_ = StaticMeshInstance::Create(scene_.get(), arrow_mesh_z_.Raw(),
                original_arrow_instance->GetTransform().RotatedAbout(glm::radians(-90.0f), {0, 1, 0}).Scaled(glm::vec3(scale)));
        }
        arrow_mesh_x_instance_->SetVisible(false);
        arrow_mesh_y_instance_->SetVisible(false);
        arrow_mesh_z_instance_->SetVisible(false);
        original_arrow_instance.SafeRelease();
    }
    auto & rhi = RHI::Get();
    auto load_gltf = [&](const std::filesystem::path& model_path, const char* name) {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        std::vector<TRef<RenderableNode>> nodes;
        std::vector<TRef<StaticMeshInstance>> new_meshes;
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator_,
            *scene_,
                renderable_node_registry_.Raw(),
            default_material_.Raw(),
            geometries, materials, new_meshes, &nodes
        )) {
            MI_WARN("Failed to load GLTF model {}.", model_path.string());
            return;
        }
        RegisterLoadedScene(name ? name : model_path.filename().string(), nodes);
        auto & r = Renderer::Get();
        for (auto e : new_meshes) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
        }
    };

    switch(cfg.default_scene_type) {
        case MESH_ONLY : {
            // auto model_path = std::filesystem::path("D:/TestScene/room/Room.gltf");
            auto model_path = std::filesystem::path("D:/TestScene/CartoonRoom/scene.gltf");
            load_gltf(model_path, "default");
            break;
        }
        case MESH_AND_VOLUME_PRIMITIVES : {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room_empty/scene.gltf");
            load_gltf(model_path, "light_room_empty");
             TRef<VolumePrimitives> volprims;
             VolumePrimitivesLoader::LoadPLY(
                 GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/puppy/point_cloud.ply"),
                 // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/grid/point_cloud.ply"),
                 // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/simple_volume/point_cloud_1point.ply"),
                 // GetInfra().TranslateResPathToFilePath("C:/Users/hineven/CLionProjects/3DGS_GI/data/armadillo/point_cloud/iteration_35000/point_cloud.ply"),
                 // GetInfra().TranslateResPathToFilePath("C:/Users/hineven/CLionProjects/3DGS_GI/data/barn/point_cloud/iteration_50000/point_cloud.ply"),
                 *resource_allocator_, volprims//, 0.1f
             );
             if (volprims) {
                 volprims->UpdateOnDevice(resource_allocator_.Raw());
                 auto volprims_instance = VolumePrimitivesInstance::Create(scene_.get(), volprims.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
                 volprims_instance->EditTransform().Translate({0, 0.5, 0});
                 // volprims_instance->EditTransform().Scale({0.1f, 0.1f, 0.1f});
             }
            break;
        }
        case MESH_AND_VOLUME_GRID : {
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room_empty/scene_.gltf");
            load_gltf(model_path, "light_room_empty_vdb");
             TRef<VolumeGrid> volume_grid;
             volume_grid = OpenVDBLoader::LoadVDB(
                 // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/bunny/bunny_density.vdb"),
                 // GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/bunny/bunny_density_color.vdb"),
                 // GetInfra().TranslateResPathToFilePath("D:/Coding/Houdini/Houdini_Works/bunny_density.vdb"),
                 GetInfra().TranslateResPathToFilePath("D:/Coding/Houdini/Houdini_Works/bunny_density_color.vdb"),
                 *resource_allocator_, {1024, "density", "color"}
             );
             if(volume_grid) {
                 volume_grid->UpdateOnDevice(resource_allocator_.Raw());
                 auto volume_grid_instance = VolumeGridInstance::Create(scene_.get(), volume_grid.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
             }
            break;
        }
        case GAUSSIAN_RADIANCE_FIELD : {
             TRef<GaussianRadianceField> field;
             if (!GaussianRadianceFieldLoader::LoadPLY("F:/CLionProjects/3DGS_GI/data/counter/point_cloud/iteration_30000/point_cloud.ply",
                 *resource_allocator_, field)) {
                 MI_WARN("Failed to load Gaussian Radiance Field PLY.");
                 }
             if (field) {
                 field->UpdateOnDevice(resource_allocator_.Raw());
                 auto inst = GaussianRadianceFieldInstance::Create(scene_.get(), field.Raw());
             }
            break;
         }
        case NONE : {
            // Do nothing
            MI_INFO("No preset scene loaded.");
            break;
        }
         default : break;
     }

    scene_->SetSkyCube(sky_cube_.Raw());
    scene_->CreateOnDevice();

    view_ = std::make_unique<RendererView>();
    view_->film_width_ = cfg.window_width;
    view_->film_height_ = cfg.window_height;
    view_->scene_ = scene_.get();

    scene_->directional_light_.direction = glm::normalize(glm::vec3(-5.5f, -4.4f, 5.5f));
}

MI_NAMESPACE_END