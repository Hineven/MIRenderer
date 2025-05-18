/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <map>
#include <set>

#include "util/gltf_loader.h"

#define CGLTF_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable:4789)
#include <cgltf.h>
#include <glm/detail/type_quat.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "core/infra.h"
#include "renderer/mi_material.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_texture.h"
#include "util/texture_loader.h"
#pragma warning(pop)

MI_NAMESPACE_BEGIN

bool GLTFLoader::LoadGLTF(
    std::filesystem::path path, CommonGroupedDeviceResourceAllocator &allocator,
    RendererScene &world,
    std::vector<TRef<Geometry> > &out_geometries, std::vector<TRef<Material> > &out_materials, std::vector<TRef<StaticMesh> > &out_meshes
) {
    assert(out_geometries.empty() && out_materials.empty() && out_meshes.empty() && "Outputs should be empty");
    assert(!path.empty());
    cgltf_options options = {};
    cgltf_data *gltf_model = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &gltf_model);
    if(result != cgltf_result_success) {
        MI_WARN("GLTFLoader: Failed to parse GLTF file {}.", path.string());
        return false;
    }
    result = cgltf_load_buffers(&options, gltf_model, path.string().c_str());
    if(result != cgltf_result_success) {
        MI_WARN("GLTFLoader: Failed to load buffers for GLTF file {}.", path.string());
        return false;
    }
#ifndef NDEBUG
    if(cgltf_validate(gltf_model) != cgltf_result_success) {
        MI_WARN("GLTFLoader: Invalid GLTF file {}.", path.string());
        return false;
    }
#endif //! NDEBUG
    if(gltf_model->scenes_count == 0) return true;

    if (gltf_model->skins_count > 0) {
        MI_WARN("GLTFLoader: Omitting {} skins.", gltf_model->skins_count);
    }
    // std::map<cgltf_skin const *, GfxConstRef<GfxSkin>> skins;
    // for(size_t i = 0; i < gltf_model->skins_count; ++i)
    // {
    //     cgltf_skin const &gltf_skin = gltf_model->skins[i];
    //     GfxRef<GfxSkin> skin_ref = gfxSceneCreateSkin(scene);
    //     skin_ref->joint_matrices.resize(gltf_skin.joints_count);
    //     GfxMetadata &skin_metadata = skin_metadata_[skin_ref];
    //     skin_metadata.asset_file = asset_file;    // set up metadata
    //     skin_metadata.object_name = (gltf_skin.name != nullptr) ? gltf_skin.name : "Skin" + std::to_string(i);
    //     skins[&gltf_skin] = skin_ref;
    // }
    if (gltf_model->cameras_count > 0) {
        MI_WARN("GLTFLoader: Omitting {} cameras.", gltf_model->cameras_count);
    }
    // std::map<cgltf_camera const *, GfxConstRef<GfxCamera>> cameras;
    // for(size_t i = 0; i < gltf_model->cameras_count; ++i)
    // {
    //     cgltf_camera const &gltf_camera = gltf_model->cameras[i];
    //     if(gltf_camera.type != cgltf_camera_type_perspective) continue; // unsupported camera type
    //     cgltf_camera_perspective const &gltf_perspective_camera = gltf_camera.data.perspective;
    //     GfxRef<GfxCamera> camera_ref = gfxSceneCreateCamera(scene);
    //     GfxCamera &camera = *camera_ref;
    //     camera.type = kGfxCameraType_Perspective;
    //     TransformGltfCamera(camera, glm::dmat4(1.0));
    //     camera.aspect = gltf_perspective_camera.has_aspect_ratio ? gltf_perspective_camera.aspect_ratio : 1.0f;
    //     camera.fovY   = gltf_perspective_camera.yfov;
    //     camera.nearZ  = gltf_perspective_camera.znear;
    //     camera.farZ   = gltf_perspective_camera.zfar;
    //     GfxMetadata &camera_metadata = camera_metadata_[camera_ref];
    //     camera_metadata.asset_file = asset_file;    // set up metadata
    //     camera_metadata.object_name = (gltf_camera.name != nullptr) ? gltf_camera.name : "Camera" + std::to_string(i);
    //     cameras[&gltf_camera] = camera_ref;
    // }
    if (gltf_model->lights_count > 0) {
        MI_WARN("GLTFLoader: Omitting {} lights.", gltf_model->lights_count);
    }
    // std::map<cgltf_light const *, GfxConstRef<GfxLight>> lights;
    // for(size_t i = 0; i < gltf_model->lights_count; ++i)
    // {
    //     cgltf_light const &gltf_light = gltf_model->lights[i];
    //     GfxRef<GfxLight> light_ref = gfxSceneCreateLight(scene);
    //     GfxLight &light = *light_ref;
    //     light.color = glm::vec3(gltf_light.color[0], gltf_light.color[1], gltf_light.color[2]);
    //     float const lumens_to_watts = 683.f;
    //     light.intensity             = gltf_light.intensity / lumens_to_watts;
    //     if(gltf_light.type == cgltf_light_type_point || gltf_light.type == cgltf_light_type_spot)
    //         light.intensity *= 4.0f * 3.1415926535897932384626433832795f;
    //     light.range = gltf_light.range > 0.0 ? gltf_light.range : FLT_MAX;
    //     light.type = gltf_light.type == cgltf_light_type_point ? kGfxLightType_Point :
    //         (gltf_light.type == cgltf_light_type_spot ? kGfxLightType_Spot : kGfxLightType_Directional);
    //     if(light.type == kGfxLightType_Spot)
    //     {
    //         light.inner_cone_angle = gltf_light.spot_inner_cone_angle;
    //         light.outer_cone_angle = gltf_light.spot_outer_cone_angle;
    //     }
    //     TransformGltfLight(light, glm::dmat4(1.0));
    //     GfxMetadata &light_metadata = light_metadata_[light_ref];
    //     light_metadata.asset_file = asset_file;    // set up metadata
    //     light_metadata.object_name = (gltf_light.name != nullptr) ? gltf_light.name : "Light" + std::to_string(i);
    //     lights[&gltf_light] = light_ref;
    // }
    std::map<cgltf_image const *, TRef<Texture>> images;
    for(size_t i = 0; i < gltf_model->textures_count; ++i)
    {
        cgltf_texture const &gltf_texture = gltf_model->textures[i];
        if(gltf_texture.image == nullptr && gltf_texture.basisu_image == nullptr)
            continue;
        cgltf_image const *gltf_image = gltf_texture.has_basisu ? gltf_texture.basisu_image : gltf_texture.image;
        TRef<Texture> image_ref;
        if(gltf_image->uri != nullptr)
        {
            auto folder = path.parent_path();
            auto image_file = folder / gltf_image->uri;
            image_ref = TextureLoader::LoadFromFile(gltf_image->name, image_file);
        }
        else if(gltf_image->buffer_view != nullptr)
        {
            const std::string mime(gltf_image->mime_type);
            if(mime != "image/jpeg" && mime != "image/png")
            {
                MI_WARN("Unsupported embedded texture type '{}' for {}", mime.c_str(), gltf_image->name);
                continue;
            }
            void *ptr = (uint8_t*)gltf_image->buffer_view->buffer->data + gltf_image->buffer_view->offset;
            image_ref = TextureLoader::LoadFromBuffer(gltf_image->name, gltf_image->mime_type, ptr, gltf_image->buffer_view->size);
        }
        images[gltf_image] = image_ref;
    }
    std::map<cgltf_material const *, TRef<Material>> materials;
    // std::map<cgltf_image const *, std::pair<TRef<Texture>, TRef<Texture>>> maps;
    for(size_t i = 0; i < gltf_model->materials_count; ++i)
    {
        std::map<cgltf_image const *, TRef<Texture>>::const_iterator it;
        cgltf_material const &gltf_material = gltf_model->materials[i];
        if(!gltf_material.has_pbr_metallic_roughness)
            continue; // unsupported material type
        TRef<Material> material_ref = Material::Create((gltf_material.name != nullptr) ? gltf_material.name : "Material" + std::to_string(i));
        cgltf_pbr_metallic_roughness const &gltf_material_pbr = gltf_material.pbr_metallic_roughness;
        material_ref->SetAlbedo(
            {gltf_material_pbr.base_color_factor[0], gltf_material_pbr.base_color_factor[1],
            gltf_material_pbr.base_color_factor[2], gltf_material_pbr.base_color_factor[3]}
        );
        material_ref->SetRoughness(gltf_material_pbr.roughness_factor);
        // material.metallicity = gltf_material_pbr.metallic_factor;
        float const emissiveFactor = gltf_material.emissive_strength.emissive_strength;
        auto emissive = glm::vec3({
            gltf_material.emissive_factor[0], gltf_material.emissive_factor[1],
            gltf_material.emissive_factor[2]
        });
        if(emissiveFactor > 0.0f)
            emissive *= emissiveFactor;
        material_ref->SetEmissive(emissive);
        // if(gltf_material.has_ior)
        //     material.ior = gltf_material.ior.ior;
        // if(gltf_material.has_specular)
        //     material.specular_ = glm::vec4(gltf_material.specular.specular_color_factor[0], gltf_material.specular.specular_color_factor[1], gltf_material.specular.specular_color_factor[2], gltf_material.specular.specular_factor);
        // if(gltf_material.has_transmission)
        //     material.transmission = gltf_material.transmission.transmission_factor;
        // if(gltf_material.has_sheen)
        //     material.sheen = glm::vec4(gltf_material.sheen.sheen_color_factor[0], gltf_material.sheen.sheen_color_factor[1], gltf_material.sheen.sheen_color_factor[2], gltf_material.sheen.sheen_roughness_factor);
        // if(gltf_material.has_clearcoat)
        // {
        //     material.clearcoat = gltf_material.clearcoat.clearcoat_factor;
        //     material.clearcoat_roughness = gltf_material.clearcoat.clearcoat_roughness_factor;
        // }
        if(gltf_material.double_sided) material_ref->SetDoubleSided(true);
        cgltf_texture const *albedo_map_text = gltf_material_pbr.base_color_texture.texture;
        it = (albedo_map_text != nullptr ? images.find(albedo_map_text->basisu_image != nullptr ?
              albedo_map_text->basisu_image : albedo_map_text->image) : images.end());
        if(it != images.end())
        {
            auto *temp = (*it).second.Raw();
            material_ref->SetAlbedoTexture((*it).second.Raw());
        }
        cgltf_texture const *metallicity_roughness_map_text = gltf_material_pbr.metallic_roughness_texture.texture;
        it = (metallicity_roughness_map_text != nullptr ? images.find(metallicity_roughness_map_text->basisu_image != nullptr ?
              metallicity_roughness_map_text->basisu_image : metallicity_roughness_map_text->image) : images.end());
        if(it != images.end())
        {
            material_ref->SetMetallicRoughnessTexture(it->second.Raw());
        }
        cgltf_texture const *emissivity_map_text = gltf_material.emissive_texture.texture;
        it = (emissivity_map_text != nullptr ? images.find(emissivity_map_text->basisu_image != nullptr ?
              emissivity_map_text->basisu_image : emissivity_map_text->image) : images.end());
        if(it != images.end())
        {

            material_ref->SetEmissiveTexture((*it).second.Raw());
        }
        // if(gltf_material.has_specular)
        // {
        //     cgltf_texture const *specular_map_text = gltf_material.specular.specular_color_texture.texture;
        //     it = (specular_map_text != nullptr ? images.find(specular_map_text->basisu_image != nullptr ?
        //           specular_map_text->basisu_image : specular_map_text->image) : images.end());
        //     if(it != images.end())
        //     {
        //         GfxImage *temp = gfxSceneGetObject<GfxImage>(scene, (*it).second);
        //         if(temp->bytes_per_channel <= 1)
        //             temp->format = ConvertImageFormatSRGB(temp->format);
        //         material.specular_map = (*it).second;
        //     }
        //     if(gltf_material.specular.specular_texture.texture != nullptr &&
        //        gltf_material.specular.specular_texture.texture != specular_map_text)
        //         GFX_PRINT_ERROR(kGfxResult_InvalidOperation,
        //             "Specular factor texture should be stored in Specular color texture alpha channel");
        // }
        cgltf_texture const *normal_map_text = gltf_material.normal_texture.texture;
        it = (normal_map_text != nullptr ? images.find(normal_map_text->basisu_image != nullptr ?
              normal_map_text->basisu_image : normal_map_text->image) : images.end());
        if(it != images.end())
        {
            material_ref->SetNormalTexture((*it).second.Raw());
        }
        // if(gltf_material.has_transmission)
        // {
        //     cgltf_texture const *transmission_map_text = gltf_material.transmission.transmission_texture.texture;
        //     it = (transmission_map_text != nullptr ? images.find(transmission_map_text->basisu_image != nullptr ?
        //           transmission_map_text->basisu_image : transmission_map_text->image) : images.end());
        //     if(it != images.end())
        //     {
        //         GfxImage *temp = gfxSceneGetObject<GfxImage>(scene, (*it).second);
        //         temp->format = ConvertImageFormatLinear(temp->format);
        //         material.transmission_map = (*it).second;
        //     }
        // }
        // if(gltf_material.has_sheen)
        // {
        //     cgltf_texture const *sheen_map_text = gltf_material.sheen.sheen_color_texture.texture;
        //     it = (sheen_map_text != nullptr ? images.find(sheen_map_text->basisu_image != nullptr ?
        //           sheen_map_text->basisu_image : sheen_map_text->image) : images.end());
        //     if(it != images.end())
        //     {
        //         GfxImage *temp = gfxSceneGetObject<GfxImage>(scene, (*it).second);
        //         if(temp->bytes_per_channel <= 1)
        //             temp->format = ConvertImageFormatSRGB(temp->format);
        //         material.sheen_map = (*it).second;
        //     }
        //     if(gltf_material.sheen.sheen_roughness_texture.texture != nullptr &&
        //        gltf_material.sheen.sheen_roughness_texture.texture != sheen_map_text)
        //         GFX_PRINT_ERROR(kGfxResult_InvalidOperation,
        //             "Sheen roughness texture should be stored in Sheen color texture alpha channel");
        // }
        // if(gltf_material.has_clearcoat)
        // {
        //     cgltf_texture const *clearcoat_map_text = gltf_material.clearcoat.clearcoat_texture.texture;
        //     it = (clearcoat_map_text != nullptr ? images.find(clearcoat_map_text->basisu_image != nullptr ?
        //           clearcoat_map_text->basisu_image : clearcoat_map_text->image) : images.end());
        //     if(it != images.end())
        //     {
        //         GfxImage *temp = gfxSceneGetObject<GfxImage>(scene, (*it).second);
        //         temp->format = ConvertImageFormatLinear(temp->format);
        //         material.clearcoat_map = (*it).second;
        //     }
        //     cgltf_texture const *clearcoat_rough_map_text = gltf_material.clearcoat.clearcoat_roughness_texture.texture;
        //     it = (clearcoat_rough_map_text != nullptr ? images.find(clearcoat_rough_map_text->basisu_image != nullptr ?
        //           clearcoat_rough_map_text->basisu_image : clearcoat_rough_map_text->image) : images.end());
        //     if(it != images.end())
        //     {
        //         GfxImage *temp = gfxSceneGetObject<GfxImage>(scene, (*it).second);
        //         temp->format = ConvertImageFormatLinear(temp->format);
        //         material.clearcoat_roughness_map = (*it).second;
        //     }
        // }
        // cgltf_texture const *ao_map_text = gltf_material.occlusion_texture.texture;
        // it = (ao_map_text != nullptr ? images.find(ao_map_text->basisu_image != nullptr ?
        //       ao_map_text->basisu_image : ao_map_text->image) : images.end());
        // if(it != images.end()) material.ao_map = (*it).second;
        materials[&gltf_material] = material_ref;
    }
    typedef std::pair<TRef<Geometry>, TRef<Material>> instance_pair;
    std::map<cgltf_mesh const *, std::vector<instance_pair>>          meshes;
    std::map<cgltf_accessor const *, TRef<Geometry>>            meshInstances;
    for(size_t i = 0; i < gltf_model->meshes_count; ++i)
    {
        cgltf_mesh const &gltf_mesh = gltf_model->meshes[i];
        std::vector<instance_pair> &mesh_list = meshes[&gltf_mesh];
        for(size_t j = 0; j < gltf_mesh.primitives_count; ++j)
        {
            cgltf_primitive const &gltf_primitive = gltf_mesh.primitives[j];
            if(gltf_primitive.targets_count > 0) continue;   // morph targets aren't supported
            if(gltf_primitive.type != cgltf_primitive_type_triangles) continue;    // only support triangle meshes
            TRef<Geometry> current_mesh;
            cgltf_attribute *gltf_primitive_attributes_end = gltf_primitive.attributes + gltf_primitive.attributes_count;
            cgltf_attribute const *it = std::find_if(gltf_primitive.attributes, gltf_primitive_attributes_end,
                [&](const cgltf_attribute& x) { return x.type == cgltf_attribute_type_position; });  // locate position stream
            cgltf_accessor const *position_buffer = it != gltf_primitive_attributes_end ? it->data : nullptr;
            if(position_buffer == nullptr) continue; // invalid mesh primitive
            std::map<cgltf_accessor const *, TRef<Geometry>>::const_iterator mesh_it = meshInstances.find(position_buffer);//Note: assumes primitives with same position buffer will always have same attributes
            if(mesh_it == meshInstances.end())
            {
                cgltf_accessor const *index_buffer = gltf_primitive.indices;
                it = std::find_if(gltf_primitive.attributes, gltf_primitive_attributes_end,
                    [&](const cgltf_attribute& x) { return x.type == cgltf_attribute_type_normal; });  // locate normal stream
                cgltf_accessor const *normal_buffer = it != gltf_primitive_attributes_end ? it->data : nullptr;
                it = std::find_if(gltf_primitive.attributes, gltf_primitive_attributes_end,
                    [&](const cgltf_attribute& x) { return x.type == cgltf_attribute_type_texcoord; });  // locate uv stream
                cgltf_accessor const *uv_buffer = it != gltf_primitive_attributes_end ? it->data : nullptr;
                it = std::find_if(gltf_primitive.attributes, gltf_primitive_attributes_end,
                    [&](const cgltf_attribute& x) { return x.type == cgltf_attribute_type_joints; });  // joints stream
                cgltf_accessor const *joints_buffer = it != gltf_primitive_attributes_end ? it->data : nullptr;
                it = std::find_if(gltf_primitive.attributes, gltf_primitive_attributes_end,
                    [&](const cgltf_attribute& x) { return x.type == cgltf_attribute_type_weights; });  // weights stream
                cgltf_accessor const *weights_buffer = it != gltf_primitive_attributes_end ? it->data : nullptr;
                std::vector<cgltf_float> sparse_position_buffer;
                std::vector<cgltf_float> sparse_normal_buffer;
                std::vector<cgltf_float> sparse_uv_buffer;
                std::vector<cgltf_float> sparse_joints_buffer;
                std::vector<cgltf_float> sparse_weights_buffer;
                std::vector<cgltf_float> sparse_index_buffer;
                auto unpack_sparse = [](cgltf_accessor const *buffer,
                                        std::vector<cgltf_float> &sparse_buffer) -> bool {
                    if(buffer != nullptr && buffer->is_sparse)
                    {
                        cgltf_size buffer_size = cgltf_num_components(buffer->type) * buffer->count;
                        sparse_buffer.resize(buffer_size);
                        if(cgltf_accessor_unpack_floats(buffer, &sparse_buffer[0], buffer_size) < buffer_size)
                        {
                            MI_WARN("Failed to unpack sparse accessor");
                            return false;
                        }
                    }
                    return true;
                };
                bool failed_unpack = !unpack_sparse(position_buffer, sparse_position_buffer);
                failed_unpack      = failed_unpack || !unpack_sparse(normal_buffer, sparse_normal_buffer);
                failed_unpack      = failed_unpack || !unpack_sparse(uv_buffer, sparse_uv_buffer);
                failed_unpack      = failed_unpack || !unpack_sparse(joints_buffer, sparse_joints_buffer);
                failed_unpack      = failed_unpack || !unpack_sparse(weights_buffer, sparse_weights_buffer);
                failed_unpack      = failed_unpack || !unpack_sparse(index_buffer, sparse_index_buffer);
                if(failed_unpack)
                {
                    continue;
                }
                bool skinned_mesh = joints_buffer != nullptr;
                TRef<Geometry> mesh_ref = Geometry::Create();
                Geometry &mesh = *mesh_ref;
                std::vector<DefaultStaticMeshVertex> mesh_vertices;
                std::vector<uint32_t> mesh_indices;
                auto unpack_vertex = [&](size_t const gltf_index) {
                    DefaultStaticMeshVertex vertex = {};
                    if(sparse_position_buffer.empty())
                    {
                        cgltf_accessor_read_float(
                            position_buffer, gltf_index, (float *)&vertex.position, sizeof(glm::vec3));
                    }
                    else
                    {
                        uint8_t *element = (uint8_t *)sparse_position_buffer.data();
                        element += position_buffer->offset + position_buffer->stride * gltf_index;
                        cgltf_element_read_float(element, position_buffer->type,
                                position_buffer->component_type, position_buffer->normalized,
                                (float *)&vertex.position, sizeof(glm::vec3));
                    }
                    if(normal_buffer != nullptr)
                    {
                        if(sparse_normal_buffer.empty())
                        {
                            cgltf_accessor_read_float(
                                normal_buffer, gltf_index, (float *)&vertex.normal, sizeof(glm::vec3));
                        }
                        else
                        {
                            uint8_t *element = (uint8_t *)sparse_normal_buffer.data();
                            element += normal_buffer->offset + normal_buffer->stride * gltf_index;
                            cgltf_element_read_float(element, normal_buffer->type,
                                    normal_buffer->component_type, normal_buffer->normalized,
                                    (float *)&vertex.normal, sizeof(glm::vec3));
                        }
                    }
                    if(uv_buffer != nullptr)
                    {
                        if(sparse_uv_buffer.empty())
                        {
                            cgltf_accessor_read_float(
                                uv_buffer, gltf_index, (float *)&vertex.uv, sizeof(glm::vec2));
                        }
                        else
                        {
                            uint8_t *element = (uint8_t *)sparse_uv_buffer.data();
                            element += uv_buffer->offset + uv_buffer->stride * gltf_index;
                            cgltf_element_read_float(element, uv_buffer->type, uv_buffer->component_type,
                                    uv_buffer->normalized, (float *)&vertex.uv, sizeof(glm::vec2));
                        }
                    }
                    // if(skinned_mesh)
                    // {
                    //     GfxJoint joint = {};
                    //
                    //     if(joints_buffer != nullptr)
                    //     {
                    //         if(sparse_joints_buffer.empty())
                    //         {
                    //             cgltf_accessor_read_uint(
                    //                 joints_buffer, gltf_index, (std::uint32_t *)&joint.joints, sizeof(glm::uvec4));
                    //         }
                    //         else
                    //         {
                    //             uint8_t *element = (uint8_t *)sparse_joints_buffer.data();
                    //             element += joints_buffer->offset + joints_buffer->stride * gltf_index;
                    //             cgltf_element_read_uint(element, joints_buffer->type, joints_buffer->component_type,
                    //                 (std::uint32_t *)&joint.joints, sizeof(glm::uvec4));
                    //         }
                    //     }
                    //
                    //     if(weights_buffer != nullptr)
                    //     {
                    //         if(sparse_weights_buffer.empty())
                    //         {
                    //             cgltf_accessor_read_float(
                    //                 weights_buffer, gltf_index, (float *)&joint.weights, sizeof(glm::vec4));
                    //         }
                    //         else
                    //         {
                    //             uint8_t *element = (uint8_t *)sparse_weights_buffer.data();
                    //             element += weights_buffer->offset + weights_buffer->stride * gltf_index;
                    //             cgltf_element_read_float(element, weights_buffer->type, weights_buffer->component_type,
                    //                 weights_buffer->normalized, (float *)&joint.weights, sizeof(glm::vec4));
                    //         }
                    //     }
                    //
                    //     mesh.joints.push_back(joint);
                    // }
                    // uint32_t const index = (uint32_t)mesh.vertices.size();
                    // if(index == 0)
                    // {
                    //     mesh.bounds_min = vertex.position;
                    //     mesh.bounds_max = vertex.position;
                    // }
                    // else
                    // {
                    //     mesh.bounds_min = glm::min(mesh.bounds_min, vertex.position);
                    //     mesh.bounds_max = glm::max(mesh.bounds_max, vertex.position);
                    // }
                    // mesh.vertices.push_back(vertex);
                    // mesh.indices.push_back(index);
                    mesh_indices.push_back((uint32_t)mesh_vertices.size());
                    mesh_vertices.push_back(vertex);
                };
                if(index_buffer != nullptr)
                {
                    std::map<size_t, uint32_t> indices;
                    for(size_t k = 0; k < index_buffer->count; ++k)
                    {
                        size_t const gltf_index = cgltf_accessor_read_index(index_buffer, k);
                        std::map<size_t, uint32_t>::const_iterator const it2 = indices.find(gltf_index);
                        if(it2 != indices.end())
                            mesh_indices.push_back((*it2).second);
                        else
                        {
                            unpack_vertex(gltf_index);
                            indices[gltf_index] = mesh_indices.back();
                        }
                    }
                }
                else
                {
                    size_t const count = position_buffer->count / 3 * 3;
                    for(size_t k = 0; k < count; ++k)
                    {
                        unpack_vertex(k);
                    }
                }
                // GfxMetadata &mesh_metadata = mesh_metadata_[mesh_ref];
                // mesh_metadata.asset_file = asset_file;  // set up metadata
                // mesh_metadata.object_name = (gltf_mesh.name != nullptr) ? gltf_mesh.name : "Mesh" + std::to_string(i);
                std::string mesh_name = (gltf_mesh.name != nullptr) ? gltf_mesh.name : "Mesh" + std::to_string(i);
                if(j > 0)
                {
                    mesh_name += ".";
                    mesh_name += std::to_string(j);
                }
                mesh_ref->SetName(mesh_name);
                current_mesh                   = mesh_ref;
                meshInstances[position_buffer] = mesh_ref;
            }
            else
            {
                current_mesh = mesh_it->second;
            }
            TRef<Material> material;
            if(gltf_primitive.material != nullptr)
            {
                std::map<cgltf_material const *, TRef<Material>>::const_iterator const it2 =
                    materials.find(gltf_primitive.material);
                if(it2 != materials.end()) material = (*it2).second;
            }
            mesh_list.push_back(std::make_pair(current_mesh, material));
        }
    }
    // std::map<cgltf_node const *, std::set<TRef<Animation>>> node_animations;
    // std::map<cgltf_node const *, uint64_t /*gfx node handle*/>        animated_nodes;
    // std::map<size_t /*gltf ID*/, GfxConstRef<GfxAnimation>>           animations;
    // for(size_t i = 0; i < gltf_model->animations_count; ++i)
    // {
    //     GfxRef<GfxAnimation> animation_ref;
    //     GltfAnimation *animation_object = nullptr;
    //     cgltf_animation const &gltf_animation = gltf_model->animations[i];
    //     for(size_t j = 0; j < gltf_animation.channels_count; ++j)
    //     {
    //         uint64_t animated_node_handle;
    //         cgltf_animation_channel const &gltf_animation_channel = gltf_animation.channels[j];
    //         if(gltf_animation_channel.target_node == nullptr) continue;
    //         GltfAnimationChannelType type = kGltfAnimationChannelType_Count;
    //              if(gltf_animation_channel.target_path == cgltf_animation_path_type_translation)
    //                  type = kGltfAnimationChannelType_Translate;
    //         else if(gltf_animation_channel.target_path == cgltf_animation_path_type_rotation)
    //                  type = kGltfAnimationChannelType_Rotate;
    //         else if(gltf_animation_channel.target_path == cgltf_animation_path_type_scale)
    //                  type = kGltfAnimationChannelType_Scale;
    //         if(type == kGltfAnimationChannelType_Count) continue;   // unsupported animation channel type
    //         if(gltf_animation_channel.sampler == nullptr) continue;
    //         cgltf_animation_sampler const &gltf_animation_sampler = *gltf_animation_channel.sampler;
    //         GltfAnimationChannelMode mode = kGltfAnimationChannelMode_Count;
    //              if(gltf_animation_sampler.interpolation == cgltf_interpolation_type_linear)
    //                  mode = kGltfAnimationChannelMode_Linear;
    //         else if(gltf_animation_sampler.interpolation == cgltf_interpolation_type_step)
    //                  mode = kGltfAnimationChannelMode_Step;
    //         else if(gltf_animation_sampler.interpolation == cgltf_interpolation_type_cubic_spline)
    //                  mode = kGltfAnimationChannelMode_Count;  // not supported yet
    //         if(mode == kGltfAnimationChannelMode_Count) continue;   // unsupported animation channel mode
    //         cgltf_accessor const *input_buffer  = gltf_animation_sampler.input;
    //         cgltf_accessor const *output_buffer = gltf_animation_sampler.output;
    //         if(input_buffer == nullptr || output_buffer == nullptr ||
    //             input_buffer->count == 0 || input_buffer->count != output_buffer->count) continue;
    //         if(input_buffer->is_sparse || output_buffer->is_sparse) continue;
    //         std::map<cgltf_node const *, uint64_t>::const_iterator const it =
    //             animated_nodes.find(gltf_animation_channel.target_node);
    //         if(it != animated_nodes.end())
    //             animated_node_handle = (*it).second;
    //         else
    //         {
    //             animated_node_handle = gltf_node_handles_.allocate_handle();
    //             animated_nodes[gltf_animation_channel.target_node] = animated_node_handle;
    //             gltf_nodes_.insert(GetObjectIndex(animated_node_handle)) = {};  // flag animated node
    //             gltf_animated_nodes_.insert(GetObjectIndex(animated_node_handle)) = {};
    //             unparented_nodes.insert(animated_node_handle);
    //         }
    //         if(!animation_ref)
    //         {
    //             animation_ref = gfxSceneCreateAnimation(scene);
    //             animations[i] = animation_ref;  // insert into map
    //             animation_object = &gltf_animations_.insert(GetObjectIndex(animation_ref));
    //             GfxMetadata &animation_metadata = animation_metadata_[animation_ref];
    //             animation_metadata.asset_file = asset_file; // set up metadata
    //             animation_metadata.object_name = (gltf_animation.name != nullptr) ? gltf_animation.name : "Animation" + std::to_string(i);
    //         }
    //         node_animations[gltf_animation_channel.target_node].insert(animation_ref);
    //         GFX_ASSERT(animation_object != nullptr);
    //         animation_object->channels_.emplace_back();
    //         GltfAnimationChannel &animation_channel = animation_object->channels_.back();
    //         animation_channel.keyframes_.resize(input_buffer->count);
    //         for(uint32_t k = 0; k < input_buffer->count; ++k)
    //             cgltf_accessor_read_float(input_buffer, k, &animation_channel.keyframes_[k], sizeof(float));
    //         animation_channel.values_.resize(output_buffer->count);
    //         for(uint32_t k = 0; k < output_buffer->count; ++k)
    //             cgltf_accessor_read_float(output_buffer, k, (float*)&animation_channel.values_[k], sizeof(glm::vec4));
    //         animation_channel.node_ = animated_node_handle;
    //         animation_channel.mode_ = mode;
    //         animation_channel.type_ = type;
    //     }
    // }
    // for(std::map<cgltf_node const *, std::set<GfxConstRef<GfxAnimation>>>::const_iterator it = node_animations.begin();
    //     it != node_animations.end(); ++it)
    //     if((*it).second.size() > 1)
    //         GFX_PRINT_ERROR(kGfxResult_InternalError, "Some nodes are targeted by several animations...");
    // std::map<cgltf_node const *, std::set<GfxConstRef<GfxAnimation>>> propagated_node_animations;
    std::vector<TRef<StaticMesh>> instances;
    std::function<void (cgltf_node const *gltf_node, glm::mat4 const &parent_transform)> VisitNode
        = [&](cgltf_node const *gltf_node, glm::mat4 const &parent_transform)
    {
        if(gltf_node == nullptr)
            return ;   // out of bounds
        glm::vec3 T(0.0), S(1.0);
        glm::quat R(1.0, 0.0, 0.0, 0.0);
        if(gltf_node->has_translation) T = glm::make_vec3(gltf_node->translation);
        if(gltf_node->has_scale)       S = glm::make_vec3(gltf_node->scale);
        if(gltf_node->has_rotation)    R = glm::make_quat(gltf_node->rotation);
        glm::mat4 local_transform(1.0); // default to identity
        cgltf_node_transform_local(gltf_node, (float*)&local_transform);
        glm::mat4 const transform = parent_transform * local_transform;
        if(gltf_node->mesh != nullptr)
        {
            auto it = meshes.find(gltf_node->mesh);
            if(it != meshes.end())
                for(size_t i = 0; i < (*it).second.size(); ++i)
                {
                    TRef<StaticMesh> instance_ref = StaticMesh::Create();
                    instances.push_back(instance_ref);
                    for (auto e : (*it)) {
                        instance_ref->AddMeshPrimitive(e.first, e.second);
                    }
                    instance_ref->SetTransform(Transform::FromMatrix(transform));
                }
        }

        for(size_t i = 0; i < gltf_node->children_count; ++i)
        {
            VisitNode(gltf_node->children[i], transform);
        }
    };
    cgltf_scene const &gltf_scene = gltf_model->scene != nullptr ? *gltf_model->scene : gltf_model->scenes[0];
    for(size_t i = 0; i < gltf_scene.nodes_count; ++i)
        VisitNode(gltf_scene.nodes[i], glm::mat4(1.0));
    out_meshes = instances;
    cgltf_free(gltf_model);
    return true;
}


MI_NAMESPACE_END