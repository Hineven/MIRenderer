/*
 * Created: 2026/1/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "viewer_app.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include "renderer/mi_texture.h"
#include "util/gaussian_radiance_field_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"
#include "util/texture_loader.h"
#include "util/openvdb_loader.h"
#include "util/renderable_node.h"

MI_NAMESPACE_BEGIN

using json = nlohmann::json;

namespace {

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool ParseVec3(const json& j, glm::vec3& out_value) {
    if (!j.is_array() || j.size() != 3) {
        return false;
    }
    if (!j[0].is_number() || !j[1].is_number() || !j[2].is_number()) {
        return false;
    }
    out_value = glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    return true;
}

static bool TryGetVec3(const json& j, const char* key, glm::vec3& out_value) {
    if (!j.is_object() || !j.contains(key)) {
        return false;
    }
    return ParseVec3(j.at(key), out_value);
}

static Transform ParseTransformOrDefault(const json& j) {
    Transform t = Transform::Identity();
    if (!j.is_object()) {
        return t;
    }

    TryGetVec3(j, "position", t.position);
    if (!TryGetVec3(j, "scale", t.scale)) {
        t.scale = glm::vec3(1.0f);
    }

    if (!TryGetVec3(j, "rotation_rad", t.rotation)) {
        glm::vec3 rotation_deg;
        if (TryGetVec3(j, "rotation_deg", rotation_deg)) {
            t.rotation = glm::radians(rotation_deg);
        }
    }
    return t;
}

static std::filesystem::path ResolvePathForLoading(const std::string& path_str) {
    std::filesystem::path p(path_str);
    if (p.is_absolute()) {
        return p;
    }

    const auto infra_path = GetInfra().TranslateResPathToFilePath(path_str);
    if (std::filesystem::exists(infra_path)) {
        return infra_path;
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        auto local_path = cwd / p;
        if (std::filesystem::exists(local_path)) {
            return local_path;
        }
    }

    return p;
}

static auto scene_file_name = std::string("viewer_scene.json");
static auto sample_scene_file_name = std::string("sample_viewer_scene.json");

static std::filesystem::path GetSampleSceneConfigPath() {
    MI_WARN("Viewer scene config not found. Attempting to load sample scene config from {}.", sample_scene_file_name);
    MI_WARN("NOTE: To load a custom scene, place a '{}' file in the application source directory or next to the executable, or specify an absolute path in the command line arguments.", scene_file_name);
    return GetInfra().TranslateResPathToFilePath("applications/3d_viewer/" + sample_scene_file_name);
}

static std::filesystem::path FindViewerSceneConfigPath() {
    const auto infra_candidate = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/" + scene_file_name);
    if (std::filesystem::exists(infra_candidate)) {
        return infra_candidate;
    }

    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        return GetSampleSceneConfigPath();
    }
    auto local_candidate = cwd / scene_file_name;
    if (std::filesystem::exists(local_candidate)) {
        return local_candidate;
    }
    return GetSampleSceneConfigPath();
}

static bool ParseSceneConfigJsonString(const std::string& scene_json_str, json& out_scene_config, std::string* out_error) {
    try {
        out_scene_config = json::parse(scene_json_str, nullptr, false);
        if (out_scene_config.is_discarded()) {
            if (out_error) {
                *out_error = "json_parse_error";
            }
            out_scene_config = json::object();
            return false;
        }
    } catch (const std::exception& e) {
        if (out_error) {
            *out_error = std::string("json_parse_error: ") + e.what();
        }
        out_scene_config = json::object();
        return false;
    }
    return true;
}

static bool LoadSceneConfigFromFile(const std::filesystem::path& scene_config_path, json& out_scene_config, std::string* out_error) {
    std::ifstream ifs(scene_config_path);
    if (!ifs) {
        if (out_error) {
            *out_error = "scene_config_not_found";
        }
        return false;
    }

    try {
        ifs >> out_scene_config;
        return true;
    } catch (const std::exception& e) {
        if (out_error) {
            *out_error = std::string("json_parse_error: ") + e.what();
        }
        out_scene_config = json::object();
        return false;
    }
}

} // namespace

void ViewerApp::LoadScene(const MainLoopStartConfig& cfg) {
    scene_ = std::make_unique<Scene>();

    view_ = std::make_unique<RendererView>();
    view_->film_width_ = cfg.window_width;
    view_->film_height_ = cfg.window_height;
    view_->scene_ = scene_.get();

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
            if (meshes.empty()) {
                MI_WARN("Arrow GLTF '{}' produced no meshes.", model_path.string());
                return;
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

    if (!cfg.start_empty) {
        std::string error;
        std::filesystem::path scene_config_path = cfg.scene_config_path.empty() ? FindViewerSceneConfigPath() : cfg.scene_config_path;
        if (!LoadSceneFromConfigAbsolutePath(scene_config_path, &error, false)) {
            MI_WARN("Failed to load viewer scene config from '{}': {}. Scene loads with defaults.", scene_config_path.string(), error);
            ApplySceneConfig(json::object(), false);
        }
    } else {
        ApplySceneConfig(json::object(), false);
    }

    scene_->CreateOnDevice();
}

bool ViewerApp::LoadSceneFromConfigAbsolutePath(const std::filesystem::path& scene_config_path, std::string* out_error, bool clear_existing) {
    if (!scene_ || !resource_allocator_) {
        if (out_error) {
            *out_error = "scene_not_initialized";
        }
        return false;
    }

    WaitForSceneMutation();
    FlushSceneDelayedDestruction();

    json scene_config = json::object();
    if (!LoadSceneConfigFromFile(scene_config_path, scene_config, out_error)) {
        return false;
    }

    MI_INFO("Loaded viewer scene config from '{}'.", scene_config_path.string());
    return ApplySceneConfig(scene_config, clear_existing);
}

bool ViewerApp::LoadSceneFromConfigJsonString(const std::string& scene_json_str, std::string* out_error, bool clear_existing) {
    if (!scene_ || !resource_allocator_) {
        if (out_error) {
            *out_error = "scene_not_initialized";
        }
        return false;
    }

    WaitForSceneMutation();
    FlushSceneDelayedDestruction();

    json scene_config = json::object();
    if (!ParseSceneConfigJsonString(scene_json_str, scene_config, out_error)) {
        return false;
    }

    return ApplySceneConfig(scene_config, clear_existing);
}

bool ViewerApp::ApplySceneConfig(const nlohmann::json& scene_config, bool clear_existing) {
    if (!scene_ || !resource_allocator_) {
        return false;
    }

    if (clear_existing) {
        CleanAllRenderableNodes();
    }

    {
        sky_cube_.SafeRelease();
        std::string environment_map_path;
        if (scene_config.contains("environment_map") && scene_config["environment_map"].is_string()) {
            environment_map_path = scene_config["environment_map"].get<std::string>();
        }

        if (!environment_map_path.empty()) {
            auto sky_file = ResolvePathForLoading(environment_map_path);
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
        }
        scene_->SetSkyCube(sky_cube_.Raw());
    }

    auto & rhi = RHI::Get();

    auto load_gltf_object = [&](const std::filesystem::path& model_path,
                                const Transform& object_transform,
                                std::vector<TRef<RenderableNode>>& out_nodes) {
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

        for (auto& n : nodes) {
            if (!n->GetParent()) {
                auto t = n->GetLocalTransform();
                t.position += object_transform.position;
                t.rotation += object_transform.rotation;
                t.scale *= object_transform.scale;
                n->SetLocalTransform(t);
            }
        }

        out_nodes.insert(out_nodes.end(), nodes.begin(), nodes.end());

        auto& r = Renderer::Get();
        for (auto& e : new_meshes) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
        }
    };

    auto load_object = [&](const json& object_j, std::vector<TRef<RenderableNode>>& scene_nodes, const std::string& scene_name) {
        if (!object_j.is_object()) {
            return;
        }
        if (!object_j.contains("path") || !object_j["path"].is_string()) {
            MI_WARN("Skip object in scene '{}': missing string field 'path'.", scene_name);
            return;
        }

        const std::string object_path_str = object_j["path"].get<std::string>();
        const auto object_path = ResolvePathForLoading(object_path_str);
        const auto object_transform = ParseTransformOrDefault(object_j.value("transform", json::object()));
        std::string object_name = object_path.filename().string();
        if (object_j.contains("name") && object_j["name"].is_string()) {
            object_name = object_j["name"].get<std::string>();
        }

        json metadata = json::object();
        if (object_j.contains("metadata") && object_j["metadata"].is_object()) {
            metadata = object_j["metadata"];
        }
        if (metadata.contains("name") && metadata["name"].is_string()) {
            object_name = metadata["name"].get<std::string>();
        }

        auto register_non_gltf_renderable = [&](Renderable* renderable) {
            if (!renderable) {
                return;
            }
            auto node = renderable_node_registry_->Create(object_name);
            node->SetRenderable(renderable);
            node->SetLocalTransform(object_transform);
            node->UpdateWorldTransform();
            scene_nodes.push_back(node);
        };

        std::string loading_format;
        if (object_j.contains("loading_format") && object_j["loading_format"].is_string()) {
            loading_format = object_j["loading_format"].get<std::string>();
        } else if (metadata.contains("loading_format") && metadata["loading_format"].is_string()) {
            loading_format = metadata["loading_format"].get<std::string>();
        }
        loading_format = ToLower(loading_format);

        if (loading_format.empty()) {
            const auto ext = ToLower(object_path.extension().string());
            if (ext == ".gltf" || ext == ".glb") {
                loading_format = "gltf";
            } else if (ext == ".vdb") {
                loading_format = "vdb";
            } else if (ext == ".ply") {
                loading_format = "volume_primitives";
                MI_WARN("Object '{}' is .ply without loading_format. Defaulting to volume_primitives.", object_path.string());
            }
        }

        if (loading_format == "gltf") {
            load_gltf_object(object_path, object_transform, scene_nodes);
            return;
        }

        if (loading_format == "volume_primitives" || loading_format == "volprims") {
            TRef<VolumePrimitives> volprims;
            float percentage = 1.0f;
            if (object_j.contains("percentage") && object_j["percentage"].is_number()) {
                percentage = object_j["percentage"].get<float>();
            } else if (metadata.contains("percentage") && metadata["percentage"].is_number()) {
                percentage = metadata["percentage"].get<float>();
            }

            if (!VolumePrimitivesLoader::LoadPLY(object_path, *resource_allocator_, volprims, percentage) || !volprims) {
                MI_WARN("Failed to load volume primitives from '{}'.", object_path.string());
                return;
            }
            volprims->UpdateOnDevice(resource_allocator_.Raw());
            auto instance = VolumePrimitivesInstance::Create(scene_.get(), volprims.Raw(), object_transform);
            register_non_gltf_renderable(instance.Raw());
            return;
        }

        if (loading_format == "grf" || loading_format == "gaussian_radiance_field") {
            TRef<GaussianRadianceField> field;
            float percentage = 1.0f;
            if (object_j.contains("percentage") && object_j["percentage"].is_number()) {
                percentage = object_j["percentage"].get<float>();
            } else if (metadata.contains("percentage") && metadata["percentage"].is_number()) {
                percentage = metadata["percentage"].get<float>();
            }

            if (!GaussianRadianceFieldLoader::LoadPLY(object_path, *resource_allocator_, field, percentage) || !field) {
                MI_WARN("Failed to load Gaussian Radiance Field from '{}'.", object_path.string());
                return;
            }
            field->UpdateOnDevice(resource_allocator_.Raw());
            auto instance = GaussianRadianceFieldInstance::Create(scene_.get(), field.Raw(), object_transform);
            register_non_gltf_renderable(instance.Raw());
            return;
        }

        if (loading_format == "vdb" || loading_format == "volume_grid") {
            OpenVDBLoader::LoadOptions options {};
            const json vdb_options = metadata.contains("vdb_options") && metadata["vdb_options"].is_object()
                ? metadata["vdb_options"]
                : object_j.value("vdb_options", json::object());

            if (vdb_options.is_object()) {
                if (vdb_options.contains("max_dimension") && vdb_options["max_dimension"].is_number_unsigned()) {
                    options.MaxDimension = vdb_options["max_dimension"].get<uint32_t>();
                }
                if (vdb_options.contains("density_grid_name") && vdb_options["density_grid_name"].is_string()) {
                    options.DensityGridName = vdb_options["density_grid_name"].get<std::string>();
                }
                if (vdb_options.contains("color_grid_name") && vdb_options["color_grid_name"].is_string()) {
                    options.ColorGridName = vdb_options["color_grid_name"].get<std::string>();
                }
                if (vdb_options.contains("density_multiplier") && vdb_options["density_multiplier"].is_number()) {
                    options.DensityMultiplier = vdb_options["density_multiplier"].get<float>();
                }
                glm::vec3 default_color;
                if (TryGetVec3(vdb_options, "default_color", default_color)) {
                    options.DefaultColor = default_color;
                }
            }

            auto volume_grid = OpenVDBLoader::LoadVDB(object_path, *resource_allocator_, options);
            if (!volume_grid) {
                MI_WARN("Failed to load volume grid from '{}'.", object_path.string());
                return;
            }
            volume_grid->UpdateOnDevice(resource_allocator_.Raw());
            auto instance = VolumeGridInstance::Create(scene_.get(), volume_grid.Raw(), object_transform);
            register_non_gltf_renderable(instance.Raw());
            return;
        }

        MI_WARN("Unsupported loading_format '{}' for object '{}'.", loading_format, object_path.string());
    };

    std::vector<json> scene_entries;
    if (scene_config.contains("scenes") && scene_config["scenes"].is_array()) {
        for (const auto& s : scene_config["scenes"]) {
            scene_entries.push_back(s);
        }
    } else if (scene_config.contains("scene")) {
        if (scene_config["scene"].is_object()) {
            scene_entries.push_back(scene_config["scene"]);
        } else if (scene_config["scene"].is_array()) {
            for (const auto& s : scene_config["scene"]) {
                scene_entries.push_back(s);
            }
        }
    } else if (scene_config.contains("objects") && scene_config["objects"].is_array()) {
        scene_entries.push_back(json::object({{"name", "default"}, {"objects", scene_config["objects"]}}));
    }

    for (size_t scene_idx = 0; scene_idx < scene_entries.size(); ++scene_idx) {
        const auto& s = scene_entries[scene_idx];
        if (!s.is_object()) {
            continue;
        }
        std::string scene_name = "scene_" + std::to_string(scene_idx);
        if (s.contains("name") && s["name"].is_string()) {
            scene_name = s["name"].get<std::string>();
        }

        std::vector<TRef<RenderableNode>> nodes;
        if (s.contains("objects") && s["objects"].is_array()) {
            for (const auto& object_j : s["objects"]) {
                load_object(object_j, nodes, scene_name);
            }
        }

        if (!nodes.empty()) {
            RegisterLoadedScene(scene_name, nodes);
        }
    }

    scene_->SetSkyCube(sky_cube_.Raw());

    if (scene_config.contains("camera") && scene_config["camera"].is_object()) {
        const auto& camera_j = scene_config["camera"];
        TryGetVec3(camera_j, "position", view_->camera_.position);
        TryGetVec3(camera_j, "direction", view_->camera_.direction);
        TryGetVec3(camera_j, "up", view_->camera_.up);

        if (camera_j.contains("fov_y") && camera_j["fov_y"].is_number()) {
            view_->camera_.fov_Y = camera_j["fov_y"].get<float>();
        } else if (camera_j.contains("fov_y_deg") && camera_j["fov_y_deg"].is_number()) {
            view_->camera_.fov_Y = glm::radians(camera_j["fov_y_deg"].get<float>());
        }
        if (camera_j.contains("near_plane") && camera_j["near_plane"].is_number()) {
            view_->camera_.near_plane = camera_j["near_plane"].get<float>();
        }
        if (camera_j.contains("far_plane") && camera_j["far_plane"].is_number()) {
            view_->camera_.far_plane = camera_j["far_plane"].get<float>();
        }

        if (glm::length(view_->camera_.direction) > 0.0f) {
            view_->camera_.direction = glm::normalize(view_->camera_.direction);
        }
        if (glm::length(view_->camera_.up) > 0.0f) {
            view_->camera_.up = glm::normalize(view_->camera_.up);
        }
    }

    scene_->directional_light_.enabled = true;
    scene_->directional_light_.direction = glm::normalize(glm::vec3(-5.5f, -4.4f, 5.5f));
    scene_->directional_light_.color = glm::vec3(1.0f, 1.0f, 1.0f);
    scene_->directional_light_.intensity = 1.0f;

    if (scene_config.contains("directional_light") && scene_config["directional_light"].is_object()) {
        const auto& light_j = scene_config["directional_light"];
        if (light_j.contains("enabled") && light_j["enabled"].is_boolean()) {
            scene_->directional_light_.enabled = light_j["enabled"].get<bool>();
        }
        TryGetVec3(light_j, "direction", scene_->directional_light_.direction);
        TryGetVec3(light_j, "color", scene_->directional_light_.color);
        if (light_j.contains("intensity") && light_j["intensity"].is_number()) {
            scene_->directional_light_.intensity = light_j["intensity"].get<float>();
        }

        if (glm::length(scene_->directional_light_.direction) > 0.0f) {
            scene_->directional_light_.direction = glm::normalize(scene_->directional_light_.direction);
        } else {
            MI_WARN("Directional light in scene config has zero-length direction. Using default direction.");
            scene_->directional_light_.direction = glm::normalize(glm::vec3(-5.5f, -4.4f, 5.5f));
        }
    }

    return true;
}

MI_NAMESPACE_END