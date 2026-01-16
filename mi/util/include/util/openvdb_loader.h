/*
* Created: 2025/11/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef OPENVDB_LOADER_H
#define OPENVDB_LOADER_H

#include <filesystem>
#include <string>
#include "renderer/mi_volume_grid.h"

MI_NAMESPACE_BEGIN

class OpenVDBLoader {
public:
    struct LoadOptions {
        // 限制 3D 纹理最长边的最大分辨率 (防止显存爆炸)
        // 例如 1024 表示最长边最多 1024 个体素
        uint32_t MaxDimension = 1024;

        // 指定密度网格的名称。如果为空，尝试自动匹配 "density", "smoke", "fog" 等。
        std::string DensityGridName = "";

        // 指定颜色网格的名称。如果为空，尝试自动匹配 "color", "Cd", "temperature" 等。
        std::string ColorGridName = "";

        // 如果没有找到颜色网格，使用此默认颜色
        glm::vec3 DefaultColor = glm::vec3(1.0f);

        // 密度乘数 (在加载阶段烘焙进纹理)
        float DensityMultiplier = 100.0f;
    };

    /**
     * @brief 加载 .vdb 文件并转换为 GPU 可用的 VolumeGrid 资源
     *
     * @param path .vdb 文件路径
     * @param options 加载选项
     * @return TRef<VolumeGrid> 如果加载失败返回 nullptr
     */
    static TRef<VolumeGrid> LoadVDB(
        const std::filesystem::path& path, DeviceBindlessResourceAllocator & allocator,
        LoadOptions options = {}
    );
};

MI_NAMESPACE_END

#endif //OPENVDB_LOADER_H