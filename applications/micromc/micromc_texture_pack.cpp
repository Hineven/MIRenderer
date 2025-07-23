/*
 * Created: 2025/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "micromc_texture_pack.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "renderer/mi_texture.h"
#include "rhi/rhi_texture.h"

namespace fs = std::filesystem;

MI_NAMESPACE_BEGIN

// ==================== 静态工厂函数 ====================

TRef<TexturePack> TexturePack::CreateFromDirectory(const std::string& resource_pack_path) {
    std::vector<std::string> texture_paths;
    std::vector<std::string> block_ids;

    // 扫描资源包目录
    if (!ScanResourcePackDirectory(resource_pack_path, texture_paths, block_ids)) {
        std::cerr << "Failed to scan resource pack directory: " << resource_pack_path << std::endl;
        return nullptr;
    }

    return CreateFromTextures(texture_paths, block_ids);
}

TRef<TexturePack> TexturePack::CreateFromTextures(
    const std::vector<std::string>& texture_paths,
    const std::vector<std::string>& block_ids) {

    if (texture_paths.empty() || texture_paths.size() != block_ids.size()) {
        std::cerr << "Invalid texture paths or block IDs for TexturePack creation" << std::endl;
        return nullptr;
    }

    TRef<TexturePack> texture_pack(new TexturePack());

    if (!texture_pack->CreateAtlasFromTextures(texture_paths, block_ids)) {
        std::cerr << "Failed to create texture atlas" << std::endl;
        return nullptr;
    }

    return texture_pack;
}

// ==================== 纹理访问接口 ====================

RHITexture* TexturePack::GetAtlasRHITexture() const {
    if (!atlas_texture_) {
        return nullptr;
    }
    return atlas_texture_->GetDeviceTexture()->GetRHITexture();
}

// ==================== UV坐标查询接口 ====================

TextureUVMapping TexturePack::GetBlockUVMapping(const std::string& block_id) const {
    auto it = block_uv_mappings_.find(block_id);
    if (it != block_uv_mappings_.end()) {
        return it->second;
    }

    // 返回默认UV映射 (0,0,1,1)
    return TextureUVMapping();
}

bool TexturePack::HasBlockTexture(const std::string& block_id) const {
    return block_uv_mappings_.find(block_id) != block_uv_mappings_.end();
}

std::vector<std::string> TexturePack::GetAllBlockIds() const {
    std::vector<std::string> block_ids;
    block_ids.reserve(block_uv_mappings_.size());

    for (const auto& [block_id, uv_mapping] : block_uv_mappings_) {
        block_ids.push_back(block_id);
    }

    return block_ids;
}

// ==================== 私有实现函数 ====================

bool TexturePack::CreateAtlasFromTextures(
    const std::vector<std::string>& texture_paths,
    const std::vector<std::string>& block_ids) {

    if (texture_paths.empty()) {
        return false;
    }

    // 计算图集大小 (简单的方形排列)
    int num_textures = static_cast<int>(texture_paths.size());
    int atlas_width = static_cast<int>(std::ceil(std::sqrt(num_textures)));
    int atlas_height = (num_textures + atlas_width - 1) / atlas_width;

    const int TEXTURE_SIZE = DEFAULT_TEXTURE_SIZE;
    int atlas_pixel_width = atlas_width * TEXTURE_SIZE;
    int atlas_pixel_height = atlas_height * TEXTURE_SIZE;

    // 更新图集信息
    atlas_info_.atlas_width = atlas_width;
    atlas_info_.atlas_height = atlas_height;
    atlas_info_.pixel_width = atlas_pixel_width;
    atlas_info_.pixel_height = atlas_pixel_height;
    atlas_info_.texture_size = TEXTURE_SIZE;
    atlas_info_.texture_count = num_textures;

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

        // 确保纹理是16x16，如果不是则截断或填充
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
        TextureUVMapping uv_mapping(
            static_cast<float>(atlas_x) / atlas_width,
            static_cast<float>(atlas_y) / atlas_height,
            static_cast<float>(atlas_x + 1) / atlas_width,
            static_cast<float>(atlas_y + 1) / atlas_height
        );

        block_uv_mappings_[block_ids[i]] = uv_mapping;

        stbi_image_free(data);
    }

    // 创建纹理对象
    atlas_texture_ = Texture::Create(
        PixelFormatType::kB8G8R8A8_SRGB,
        static_cast<uint32_t>(atlas_pixel_width),
        static_cast<uint32_t>(atlas_pixel_height)
    );

    if (!atlas_texture_) {
        std::cerr << "Failed to create atlas texture" << std::endl;
        return false;
    }

    atlas_texture_->InitializeFromBinary(std::span<uint8_t>(atlas_data.data(), atlas_data.size()));
    atlas_texture_->SetName("Minecraft Block Atlas");

    // 转化成无绑定材质，便于使用
    atlas_texture_->ConvertToBindless(atlas_texture_);

    return true;
}

bool TexturePack::ScanResourcePackDirectory(
    const std::string& resource_pack_path,
    std::vector<std::string>& texture_paths,
    std::vector<std::string>& block_ids) {

    // 检查路径是否存在
    if (!fs::exists(resource_pack_path) || !fs::is_directory(resource_pack_path)) {
        std::cerr << "Resource pack directory does not exist: " << resource_pack_path << std::endl;
        return false;
    }

    texture_paths.clear();
    block_ids.clear();

    try {
        // 遍历资源包目录中的PNG文件
        for (const auto& entry : fs::directory_iterator(resource_pack_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
                std::string filename = entry.path().stem().string();
                texture_paths.push_back(entry.path().string());
                block_ids.push_back("minecraft:" + filename);
            }
        }
    }
    catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error while scanning resource pack: " << e.what() << std::endl;
        return false;
    }

    if (texture_paths.empty()) {
        std::cerr << "No PNG textures found in resource pack directory: " << resource_pack_path << std::endl;
        return false;
    }

    std::cout << "Found " << texture_paths.size() << " textures in resource pack" << std::endl;
    return true;
}

MI_NAMESPACE_END
