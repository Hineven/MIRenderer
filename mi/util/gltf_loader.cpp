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
    std::vector<TRef<Geometry> > &out_geometries,
    std::vector<TRef<Material> > &out_materials,
    std::vector<TRef<StaticMeshInstance> > &out_meshes
) {
    assert(out_geometries.empty() && out_materials.empty() && out_meshes.empty() && "Outputs should be empty");
    assert(!path.empty());
    cgltf_options options = {};
    cgltf_data *gltf_model = nullptr;
    // TODO : use Infra resource ops to open file
    cgltf_result result = cgltf_parse_file(&options, path.string().c_str(), &gltf_model);
    if(result != cgltf_result_success) {
        MI_WARN("GLTFLoader: Failed to parse GLTF file {}. Error: {}", path.string(), (uint32_t)result);
        return false;
    }
    result = cgltf_load_buffers(&options, gltf_model, path.string().c_str());
    if(result != cgltf_result_success) {
        MI_WARN("GLTFLoader: Failed to load buffers for GLTF file {}. Error: {}", path.string(), (uint32_t)result);
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
    if (gltf_model->cameras_count > 0) {
        MI_WARN("GLTFLoader: Omitting {} cameras.", gltf_model->cameras_count);
    }
    if (gltf_model->lights_count > 0) {
        MI_WARN("GLTFLoader: Omitting {} lights.", gltf_model->lights_count);
    }
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
            image_ref = TextureLoader::LoadFromFile(gltf_image->name ? gltf_image->name : "", image_file);
        }
        else if(gltf_image->buffer_view != nullptr)
        {
            const std::string mime(gltf_image->mime_type);
            if(mime != "image/jpeg" && mime != "image/png")
            {
                MI_WARN("Unsupported embedded texture type '{}' for {}", mime.c_str(), gltf_image->name ? gltf_image->name : "");
                continue;
            }
            void *ptr = (uint8_t*)gltf_image->buffer_view->buffer->data + gltf_image->buffer_view->offset;
            image_ref = TextureLoader::LoadFromBuffer(gltf_image->name ? gltf_image->name : "", gltf_image->mime_type, ptr, gltf_image->buffer_view->size);
        }
        if (image_ref) {
            image_ref->UpdateOnDevice();
            image_ref->ConvertToBindless();
        }
        images[gltf_image] = image_ref;
    }
    std::map<cgltf_material const *, TRef<Material>> materials;
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
        if(gltf_material.double_sided) material_ref->SetDoubleSided(true);
        cgltf_texture const *albedo_map_text = gltf_material_pbr.base_color_texture.texture;
        it = (albedo_map_text != nullptr ? images.find(albedo_map_text->basisu_image != nullptr ?
              albedo_map_text->basisu_image : albedo_map_text->image) : images.end());
        if(it != images.end())
        {
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
        cgltf_texture const *normal_map_text = gltf_material.normal_texture.texture;
        it = (normal_map_text != nullptr ? images.find(normal_map_text->basisu_image != nullptr ?
              normal_map_text->basisu_image : normal_map_text->image) : images.end());
        if(it != images.end())
        {
            material_ref->SetNormalTexture((*it).second.Raw());
        }
        materials[&gltf_material] = material_ref;
    }
    typedef std::pair<TRef<Geometry>, TRef<Material>> geometry_material_pair;
    std::map<cgltf_mesh const *, std::vector<geometry_material_pair>> mesh_map;
    std::map<cgltf_accessor const *, TRef<Geometry>> geometry_map;
    for(size_t i = 0; i < gltf_model->meshes_count; ++i)
    {
        cgltf_mesh const &gltf_mesh = gltf_model->meshes[i];
        std::vector<geometry_material_pair> &geometry_material_pair_list = mesh_map[&gltf_mesh];
        for(size_t j = 0; j < gltf_mesh.primitives_count; ++j)
        {
            cgltf_primitive const &gltf_primitive = gltf_mesh.primitives[j];
            if(gltf_primitive.targets_count > 0) continue;   // morph targets aren't supported
            if(gltf_primitive.type != cgltf_primitive_type_triangles) continue;    // only support triangle meshes
            TRef<Geometry> current_geometry;
            cgltf_attribute *gltf_primitive_attributes_end = gltf_primitive.attributes + gltf_primitive.attributes_count;
            cgltf_attribute const *it = std::find_if(gltf_primitive.attributes, gltf_primitive_attributes_end,
                [&](const cgltf_attribute& x) { return x.type == cgltf_attribute_type_position; });  // locate position stream
            cgltf_accessor const *position_buffer = it != gltf_primitive_attributes_end ? it->data : nullptr;
            if(position_buffer == nullptr) continue; // invalid mesh primitive
            std::map<cgltf_accessor const *, TRef<Geometry>>::const_iterator geometry_it = geometry_map.find(position_buffer);//Note: assumes primitives with same position buffer will always have same attributes
            if(geometry_it == geometry_map.end())
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
                std::vector<DefaultStaticMeshVertex> mesh_vertices;
                std::vector<uint32_t> mesh_indices;
                auto unpack_vertex = [&](size_t const gltf_index) {
                    DefaultStaticMeshVertex vertex = {};
                    if(sparse_position_buffer.empty())
                    {
                        cgltf_accessor_read_float(
                            position_buffer, gltf_index, (float *)&vertex.Position, sizeof(glm::vec3));
                    }
                    else
                    {
                        uint8_t *element = (uint8_t *)sparse_position_buffer.data();
                        element += position_buffer->offset + position_buffer->stride * gltf_index;
                        cgltf_element_read_float(element, position_buffer->type,
                                position_buffer->component_type, position_buffer->normalized,
                                (float *)&vertex.Position, sizeof(glm::vec3));
                    }
                    if(normal_buffer != nullptr)
                    {
                        if(sparse_normal_buffer.empty())
                        {
                            cgltf_accessor_read_float(
                                normal_buffer, gltf_index, (float *)&vertex.Normal, sizeof(glm::vec3));
                        }
                        else
                        {
                            uint8_t *element = (uint8_t *)sparse_normal_buffer.data();
                            element += normal_buffer->offset + normal_buffer->stride * gltf_index;
                            cgltf_element_read_float(element, normal_buffer->type,
                                    normal_buffer->component_type, normal_buffer->normalized,
                                    (float *)&vertex.Normal, sizeof(glm::vec3));
                        }
                    }
                    if(uv_buffer != nullptr)
                    {
                        if(sparse_uv_buffer.empty())
                        {
                            cgltf_accessor_read_float(
                                uv_buffer, gltf_index, (float *)&vertex.UV, sizeof(glm::vec2));
                        }
                        else
                        {
                            uint8_t *element = (uint8_t *)sparse_uv_buffer.data();
                            element += uv_buffer->offset + uv_buffer->stride * gltf_index;
                            cgltf_element_read_float(element, uv_buffer->type, uv_buffer->component_type,
                                    uv_buffer->normalized, (float *)&vertex.UV, sizeof(glm::vec2));
                        }
                    }
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
                std::string mesh_name = (gltf_mesh.name != nullptr) ? gltf_mesh.name : "Mesh" + std::to_string(i);
                if(j > 0)
                {
                    mesh_name += ".";
                    mesh_name += std::to_string(j);
                }
                TRef<Geometry> geometry_ref = Geometry::CreateFromVertices(mesh_vertices, mesh_indices);
                geometry_ref->SetName(mesh_name);
                geometry_ref->UpdateOnDevice(&allocator);
                current_geometry                   = geometry_ref;
                geometry_map[position_buffer] = geometry_ref;
            }
            else
            {
                current_geometry = geometry_it->second;
            }
            TRef<Material> material;
            if(gltf_primitive.material != nullptr)
            {
                std::map<cgltf_material const *, TRef<Material>>::const_iterator const it2 =
                    materials.find(gltf_primitive.material);
                if(it2 != materials.end()) material = (*it2).second;
            }
            geometry_material_pair_list.push_back(std::make_pair(current_geometry, material));
        }
    }
    std::vector<TRef<StaticMeshInstance>> mesh_instances;
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
            auto it = mesh_map.find(gltf_node->mesh);
            if(it != mesh_map.end())
                for(size_t i = 0; i < (*it).second.size(); ++i)
                {
                    TRef<StaticMeshInstance> instance_ref = StaticMeshInstance::Create(&world, Transform::Identity());
                    mesh_instances.push_back(instance_ref);
                    for (auto e : (it->second)) {
                        e.second->UpdateOnDevice(&allocator);
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
    out_meshes = mesh_instances;
    for (auto e : materials) {
        out_materials.push_back(e.second);
    }
    for (auto e : geometry_map) {
        out_geometries.push_back(e.second);
    }
    cgltf_free(gltf_model);
    return true;
}


MI_NAMESPACE_END