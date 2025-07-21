/*
 * Created: 2025/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>

// We linked the util lib.
// #define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "micromc.h"
#include "renderer/mi_renderer.h"
using json = nlohmann::json;

// MicroMCChunk 实现
MicroMCChunk::MicroMCChunk(int coord_x, int coord_z)
    : coord_x_(coord_x), coord_z_(coord_z) {
}

MicroMCChunk::~MicroMCChunk() {
}

void MicroMCChunk::UpdateGeometries(Scene * world, Material * block_material, const std::vector<MCBlock>& blocks,
                                   const std::unordered_map<std::string, TextureUVMapping>& uv_mappings) {
    // 生成立方体几何体
    GenerateCubeGeometry(world, block_material, blocks, uv_mappings);
    // 更新静态网格实例
    static_mesh_instance_ = StaticMeshInstance::Create(world, static_mesh_.Raw(), Transform::Identity());
}

#define SUB_CHUNK_HEIGHT 16 // 每个子区块的高度
#define MAX_NUM_SUB_CHUNKS 32 // 最大子区块数量

void MicroMCChunk::GenerateCubeGeometry(Scene * world, Material * block_material, const std::vector<MCBlock>& blocks,
                                       const std::unordered_map<std::string, TextureUVMapping>& uv_mappings) {

    std::vector<DefaultStaticMeshVertex> vertices[MAX_NUM_SUB_CHUNKS];
    std::vector<uint32_t> indices[MAX_NUM_SUB_CHUNKS];


    // 立方体的6个面的法线和切线
    struct FaceData {
        glm::vec3 normal;
        glm::vec3 tangent;
        glm::vec3 bitangent;
        glm::vec3 offset[4]; // 4个顶点的偏移
    };

    FaceData faces[6] = {
        // 前面 (+Z)
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}},
        // 后面 (-Z)
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}, {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}},
        // 右面 (+X)
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}},
        // 左面 (-X)
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {{0, 0, 1}, {0, 0, 0}, {0, 1, 0}, {0, 1, 1}}},
        // 上面 (+Y)
        {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}, {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}},
        // 下面 (-Y)
        {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}, {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}}}
    };

    // UV坐标 (顺时针)
    glm::vec2 face_uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

    for (const auto& block : blocks) {
        glm::vec3 block_pos(block.x, block.y, block.z);

        // 获取该方块的UV映射
        auto uv_it = uv_mappings.find(block.id);
        if (uv_it == uv_mappings.end()) {
            continue; // 跳过没有纹理的方块
        }

        const TextureUVMapping& uv_mapping = uv_it->second;

        // 检查6个面是否需要渲染（面部剔除）
        glm::ivec3 neighbors[6] = {
            {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}
        };

        for (int face = 0; face < 6; face++) {
            glm::ivec3 neighbor_pos = glm::ivec3(block_pos) + neighbors[face];

            // 如果邻居位置有方块，则不渲染这个面
            if (IsBlockAt(blocks, neighbor_pos.x, neighbor_pos.y, neighbor_pos.z)) {
                continue;
            }

            int sub_chunk = (block.y + 128) / SUB_CHUNK_HEIGHT;

            // 添加这个面
            AddCubeFace(vertices[sub_chunk], indices[sub_chunk], block_pos,
                       faces[face].normal, faces[face].tangent, faces[face].bitangent,
                       uv_mapping);
        }
    }
    // 重建Geometry
    geometries_.resize(MAX_NUM_SUB_CHUNKS);
    for (int i = 0; i < MAX_NUM_SUB_CHUNKS; i++) {
        if (!vertices[i].empty() && !indices[i].empty()) {
            TRef<Geometry> geometry = Geometry::CreateFromVertices(vertices[i], indices[i]);
            geometries_[i] = geometry;
            geometry->UpdateOnDevice(Renderer::Get().GetDeviceAllocator());
        } else {
            geometries_[i] = nullptr; // 清理空的几何体
        }
    }
    // 重建StaticMesh
    if (!static_mesh_) static_mesh_ = StaticMesh::Create(true, false);
    static_mesh_->ClearMeshPrimitives();
    for (int i = 0; i < MAX_NUM_SUB_CHUNKS; i++) {
        if (geometries_[i]) {
            static_mesh_->AddMeshPrimitive(geometries_[i], block_material);
        }
    }
    static_mesh_->UpdateOnDevice(Renderer::Get().GetDeviceAllocator());
}

bool MicroMCChunk::IsBlockAt(const std::vector<MCBlock>& blocks, int x, int y, int z) const {
    for (const auto& block : blocks) {
        if (block.x == x && block.y == y && block.z == z) {
            return true;
        }
    }
    return false;
}

void MicroMCChunk::AddCubeFace(std::vector<DefaultStaticMeshVertex>& vertices,
                              std::vector<uint32_t>& indices,
                              const glm::vec3& position,
                              const glm::vec3& normal,
                              const glm::vec3& tangent,
                              const glm::vec3& bitangent,
                              const TextureUVMapping& uv_mapping) {
    uint32_t base_vertex = (uint32_t)vertices.size();

    // 面的4个顶点偏移
    glm::vec3 face_offsets[4];
    if (normal.z > 0.5f) { // 前面 (+Z)
        face_offsets[0] = {0, 0, 1}; face_offsets[1] = {1, 0, 1};
        face_offsets[2] = {1, 1, 1}; face_offsets[3] = {0, 1, 1};
    } else if (normal.z < -0.5f) { // 后面 (-Z)
        face_offsets[0] = {1, 0, 0}; face_offsets[1] = {0, 0, 0};
        face_offsets[2] = {0, 1, 0}; face_offsets[3] = {1, 1, 0};
    } else if (normal.x > 0.5f) { // 右面 (+X)
        face_offsets[0] = {1, 0, 0}; face_offsets[1] = {1, 0, 1};
        face_offsets[2] = {1, 1, 1}; face_offsets[3] = {1, 1, 0};
    } else if (normal.x < -0.5f) { // 左面 (-X)
        face_offsets[0] = {0, 0, 1}; face_offsets[1] = {0, 0, 0};
        face_offsets[2] = {0, 1, 0}; face_offsets[3] = {0, 1, 1};
    } else if (normal.y > 0.5f) { // 上面 (+Y)
        face_offsets[0] = {0, 1, 0}; face_offsets[1] = {1, 1, 0};
        face_offsets[2] = {1, 1, 1}; face_offsets[3] = {0, 1, 1};
    } else { // 下面 (-Y)
        face_offsets[0] = {0, 0, 1}; face_offsets[1] = {1, 0, 1};
        face_offsets[2] = {1, 0, 0}; face_offsets[3] = {0, 0, 0};
    }

    // UV坐标
    glm::vec2 face_uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};

    // 添加4个顶点
    for (int i = 0; i < 4; i++) {
        DefaultStaticMeshVertex vertex;
        vertex.Position = position + face_offsets[i];
        vertex.Normal = normal;

        // 计算实际UV坐标
        glm::vec2 uv = face_uvs[i];
        vertex.UV.x = uv_mapping.u_min + uv.x * (uv_mapping.u_max - uv_mapping.u_min);
        vertex.UV.y = uv_mapping.v_min + uv.y * (uv_mapping.v_max - uv_mapping.v_min);

        vertices.push_back(vertex);
    }

    // 添加索引 (两个三角形)
    indices.push_back(base_vertex + 0);
    indices.push_back(base_vertex + 1);
    indices.push_back(base_vertex + 2);

    indices.push_back(base_vertex + 0);
    indices.push_back(base_vertex + 2);
    indices.push_back(base_vertex + 3);
}

// MicroMCWorld 实现
MicroMCWorld::MicroMCWorld() {
}

MicroMCWorld::~MicroMCWorld() {
}

bool MicroMCWorld::LoadFromDirectory(const std::string& world_path, const std::string& resource_pack_path, Scene* scene) {
    // 1. 加载世界数据
    MCWorld world_data;
    if (!LoadWorldData(world_path, world_data)) {
        std::cerr << "Failed to load world data from: " << world_path << std::endl;
        return false;
    }

    // 2. 加载资源包
    if (!LoadResourcePack(resource_pack_path)) {
        std::cerr << "Failed to load resource pack from: " << resource_pack_path << std::endl;
        return false;
    }

    // 3. 创建方块材质
    CreateBlockMaterial();
    // 上传材质到GPU
    block_material_->UpdateOnDevice(Renderer::Get().GetDeviceAllocator());

    // 4. 创建区块
    for (const auto& chunk_data : world_data.chunks) {
        auto chunk = std::make_unique<MicroMCChunk>(chunk_data.coord_x, chunk_data.coord_z);
        chunk->UpdateGeometries(scene, block_material_.Raw(), chunk_data.blocks, block_uv_mappings_);

        // 创建StaticMesh并添加到场景
        if (auto mesh = chunk->GetStaticMeshInstance()) {
            Transform transform;
            transform.position = glm::vec3(chunk_data.coord_x * 16, 0, chunk_data.coord_z * 16);
            mesh->SetTransform(transform);
        }

        chunks_.push_back(std::move(chunk));
    }

    MI_INFO("Created {} chunks!", chunks_.size());

    return true;
}

bool MicroMCWorld::LoadWorldData(const std::string& world_path, MCWorld& world_data) {
    std::ifstream file(world_path);
    if (!file.is_open()) {
        return false;
    }

    json j;
    file >> j;

    if (!j.contains("chunks")) {
        return false;
    }

    for (const auto& chunk_json : j["chunks"]) {
        MCChunk chunk;

        if (chunk_json.contains("coords") && chunk_json["coords"].size() >= 2) {
            chunk.coord_x = chunk_json["coords"][0];
            chunk.coord_z = chunk_json["coords"][1];
        }

        if (chunk_json.contains("blocks")) {
            for (const auto& block_json : chunk_json["blocks"]) {
                MCBlock block;
                block.x = block_json["x"];
                block.y = block_json["y"];
                block.z = block_json["z"];
                block.id = block_json["id"];
                chunk.blocks.push_back(block);
            }
        }

        world_data.chunks.push_back(chunk);
    }

    return true;
}

bool MicroMCWorld::LoadResourcePack(const std::string& resource_pack_path) {
    namespace fs = std::filesystem;

    if (!fs::exists(resource_pack_path) || !fs::is_directory(resource_pack_path)) {
        return false;
    }

    std::vector<std::string> texture_paths;
    std::vector<std::string> block_ids;

    // 遍历资源包目录中的PNG文件
    for (const auto& entry : fs::directory_iterator(resource_pack_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".png") {
            std::string filename = entry.path().stem().string();
            texture_paths.push_back(entry.path().string());
            block_ids.push_back("minecraft:" + filename);
        }
    }

    return CreateTextureAtlas(texture_paths, block_ids);
}

bool MicroMCWorld::CreateTextureAtlas(const std::vector<std::string>& texture_paths,
                                     const std::vector<std::string>& block_ids) {
    if (texture_paths.empty()) {
        return false;
    }

    // 计算图集大小 (简单的方形排列)
    int num_textures = (uint32_t)texture_paths.size();
    int atlas_width = static_cast<int>(std::ceil(std::sqrt(num_textures)));
    int atlas_height = (num_textures + atlas_width - 1) / atlas_width;

    const int TEXTURE_SIZE = 16;
    int atlas_pixel_width = atlas_width * TEXTURE_SIZE;
    int atlas_pixel_height = atlas_height * TEXTURE_SIZE;

    // 创建图集数据
    std::vector<unsigned char> atlas_data(atlas_pixel_width * atlas_pixel_height * 4, 0);

    // 加载每个纹理并复制到图集中
    for (int i = 0; i < num_textures; i++) {
        int atlas_x = i % atlas_width;
        int atlas_y = i / atlas_width;

        // 加载纹理
        int width, height, channels;
        unsigned char* data = stbi_load(texture_paths[i].c_str(), &width, &height, &channels, 4);

        if (!data) {
            std::cerr << "Failed to load texture: " << texture_paths[i] << std::endl;
            continue;
        }

        // 确保纹理是16x16，如果不是则截断
        int copy_width = std::min(width, TEXTURE_SIZE);
        int copy_height = std::min(height, TEXTURE_SIZE);

        // 复制到图集
        for (int y = 0; y < copy_height; y++) {
            for (int x = 0; x < copy_width; x++) {
                int src_offset = (y * width + x) * 4;
                int dst_x = atlas_x * TEXTURE_SIZE + x;
                int dst_y = atlas_y * TEXTURE_SIZE + y;
                int dst_offset = (dst_y * atlas_pixel_width + dst_x) * 4;

                for (int c = 0; c < 4; c++) {
                    atlas_data[dst_offset + c] = data[src_offset + c];
                }
            }
        }

        // 计算UV映射
        TextureUVMapping uv_mapping;
        uv_mapping.u_min = static_cast<float>(atlas_x) / atlas_width;
        uv_mapping.v_min = static_cast<float>(atlas_y) / atlas_height;
        uv_mapping.u_max = static_cast<float>(atlas_x + 1) / atlas_width;
        uv_mapping.v_max = static_cast<float>(atlas_y + 1) / atlas_height;

        block_uv_mappings_[block_ids[i]] = uv_mapping;

        stbi_image_free(data);
    }

    // 创建纹理对象 - 使用正确的接口

    atlas_texture_ = Texture::Create(
        PixelFormatType::kB8G8R8A8_SRGB,
        static_cast<uint32_t>(atlas_pixel_width),
        static_cast<uint32_t>(atlas_pixel_height)
    );

    atlas_texture_->InitializeFromBinary(std::span<uint8_t>(atlas_data.data(), atlas_data.size()));
    atlas_texture_->SetName("Minecraft Block Atlas");

    // 转化成无绑定材质，便于使用
    atlas_texture_->ConvertToBindless(atlas_texture_);

    return atlas_texture_ != nullptr;
}

void MicroMCWorld::CreateBlockMaterial() {
    block_material_ = Material::Create("mc_block_material");
    if (atlas_texture_) {
        block_material_->SetAlbedoTexture(atlas_texture_.Raw());
    }
}
