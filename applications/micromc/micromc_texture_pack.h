/*
 * Created: 2025/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MICROMC_TEXTURE_PACK_H
#define MICROMC_TEXTURE_PACK_H

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <filesystem>

#include "core/refcounted.h"
#include "core/base.h"
#include "core/common.h"
#include "renderer/mi_texture.h"
#include "rhi/rhi_texture.h"

MI_NAMESPACE_BEGIN

// UV坐标映射结构
struct TextureUVMapping {
    float u_min, v_min, u_max, v_max;

    TextureUVMapping() : u_min(0.0f), v_min(0.0f), u_max(1.0f), v_max(1.0f) {}
    TextureUVMapping(float u_min, float v_min, float u_max, float v_max)
        : u_min(u_min), v_min(v_min), u_max(u_max), v_max(v_max) {}
};

/**
 * TexturePack - Minecraft资源包纹理图集加载和管理器
 *
 * 功能：
 * 1. 从文件夹加载MC资源包纹理
 * 2. 创建纹理图集
 * 3. 提供方块ID到UV坐标的映射
 * 4. 管理RHI纹理资源
 */
class TexturePack : public RefCounted<>, public NonCopyable, public NonMovable {
public:
    /**
     * 从资源包文件夹创建TexturePack
     * @param resource_pack_path 资源包文件夹路径
     * @return 成功创建的TexturePack或nullptr
     */
    static TRef<TexturePack> CreateFromDirectory(const std::string& resource_pack_path);

    /**
     * 从纹理文件列表创建TexturePack
     * @param texture_paths 纹理文件路径列表
     * @param block_ids 对应的方块ID列表
     * @return 成功创建的TexturePack或nullptr
     */
    static TRef<TexturePack> CreateFromTextures(
        const std::vector<std::string>& texture_paths,
        const std::vector<std::string>& block_ids
    );

    ~TexturePack() = default;

    // ==================== 纹理访问接口 ====================

    /**
     * 获取纹理图集的RHI纹理句柄
     * @return RHI纹理指针，如果未初始化则返回nullptr
     */
    RHITexture* GetAtlasRHITexture() const;

    /**
     * 获取纹理图集的Texture对象
     * @return Texture引用，如果未初始化则返回nullptr
     */
    TRef<Texture> GetAtlasTexture() const { return atlas_texture_; }

    // ==================== UV坐标查询接口 ====================

    /**
     * 根据方块ID获取UV坐标
     * @param block_id 方块ID（如"minecraft:stone"）
     * @return UV坐标映射，如果找不到则返回默认映射(0,0,1,1)
     */
    TextureUVMapping GetBlockUVMapping(const std::string& block_id) const;

    /**
     * 检查是否包含指定方块ID的纹理
     * @param block_id 方块ID
     * @return 如果包含则返回true
     */
    bool HasBlockTexture(const std::string& block_id) const;

    /**
     * 获取所有已加载的方块ID列表
     * @return 方块ID列表
     */
    std::vector<std::string> GetAllBlockIds() const;

    // ==================== 图集信息接口 ====================

    /**
     * 获取图集尺寸信息
     */
    struct AtlasInfo {
        int atlas_width = 0;        // 图集宽度（纹理单元数）
        int atlas_height = 0;       // 图集高度（纹理单元数）
        int pixel_width = 0;        // 图集像素宽度
        int pixel_height = 0;       // 图集像素高度
        int texture_size = 16;      // 单个纹理尺寸
        int texture_count = 0;      // 纹理总数
    };

    /**
     * 获取图集信息
     * @return 图集信息结构
     */
    const AtlasInfo& GetAtlasInfo() const { return atlas_info_; }

    /**
     * 检查TexturePack是否已成功初始化
     * @return 如果初始化成功则返回true
     */
    bool IsValid() const { return atlas_texture_ != nullptr; }

private:
    TexturePack() = default;

    /**
     * 从纹理路径和方块ID创建图集
     * @param texture_paths 纹理文件路径列表
     * @param block_ids 对应的方块ID列表
     * @return 创建成功返回true
     */
    bool CreateAtlasFromTextures(
        const std::vector<std::string>& texture_paths,
        const std::vector<std::string>& block_ids
    );

    /**
     * 扫描资源包文件夹获取纹理文件
     * @param resource_pack_path 资源包路径
     * @param texture_paths 输出：纹理文件路径列表
     * @param block_ids 输出：对应的方块ID列表
     * @return 扫描成功返回true
     */
    static bool ScanResourcePackDirectory(
        const std::string& resource_pack_path,
        std::vector<std::string>& texture_paths,
        std::vector<std::string>& block_ids
    );

private:
    TRef<Texture> atlas_texture_;                                      // 纹理图集对象
    std::unordered_map<std::string, TextureUVMapping> block_uv_mappings_; // 方块ID到UV映射
    AtlasInfo atlas_info_;                                            // 图集信息

    static constexpr int DEFAULT_TEXTURE_SIZE = 16;  // 默认纹理尺寸
};

MI_NAMESPACE_END

#endif // MICROMC_TEXTURE_PACK_H
