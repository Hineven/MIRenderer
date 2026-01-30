/*
 * Created: 2025/11/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "util/openvdb_loader.h"

#include <fstream>
#include <algorithm>
#include <vector>
#include <cmath>

// OpenVDB Includes
// 屏蔽 OpenVDB 可能产生的某些编译器警告
#pragma warning(push)
#pragma warning(disable: 4244 4267)
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/ValueTransformer.h>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#pragma warning(pop)

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include "core/infra.h"
#include "core/task.h"
#include "renderer/mi_volume_texture.h"

MI_NAMESPACE_BEGIN

static bool vdb_initialized;

TRef<VolumeGrid> OpenVDBLoader::LoadVDB(
    const std::filesystem::path& path,
    DeviceBindlessResourceAllocator & allocator,
    LoadOptions options) {

    // 切换到按需初始化
    if (!vdb_initialized) {
        openvdb::initialize();
    }

    if (!std::filesystem::exists(path)) {
        MI_WARN("OpenVDBLoader: File {} does not exist.", path.string());
        return nullptr;
    }

    // 打开文件
    openvdb::io::File file(path.string());
    try {
        file.open();
    } catch (const openvdb::IoError& e) {
        MI_WARN("OpenVDBLoader: Failed to open VDB file {}: {}", path.string(), e.what());
        return nullptr;
    }

    // --- 1. 查找网格 (Grids) ---
    openvdb::GridBase::Ptr density_grid_base;
    openvdb::GridBase::Ptr color_grid_base;

    // 辅助 lambda: 按名称或模糊匹配查找网格
    auto find_grid = [&](const std::string& target_name, const std::vector<std::string>& candidates) -> openvdb::GridBase::Ptr {
        // 精确匹配
        if (!target_name.empty()) {
            if (file.hasGrid(target_name)) return file.readGrid(target_name);
        }
        // 模糊匹配候选列表
        for (const auto& candidate : candidates) {
            for (auto iter = file.beginName(); iter != file.endName(); ++iter) {
                std::string name = iter.gridName();
                // 简单的子字符串匹配
                if (name.find(candidate) != std::string::npos) {
                    return file.readGrid(name);
                }
            }
        }
        return nullptr;
    };

    // 尝试查找密度网格
    density_grid_base = find_grid(options.DensityGridName, {"density", "smoke", "fog", "volume"});
    if (!density_grid_base) {
        MI_WARN("OpenVDBLoader: Could not find density grid in {}.", path.string());
        file.close();
        return nullptr;
    }

    // 尝试查找颜色网格
    color_grid_base = find_grid(options.ColorGridName, {"color", "Cd", "temperature"});

    // 转换为具体的 FloatGrid (密度通常是 float)
    auto density_grid = openvdb::gridPtrCast<openvdb::FloatGrid>(density_grid_base);
    if (!density_grid) {
        MI_WARN("OpenVDBLoader: Density grid is not of type FloatGrid.");
        file.close();
        return nullptr;
    }

    // --- 2. 计算世界空间包围盒 ---
    // 获取 Index Space 的活跃包围盒
    openvdb::CoordBBox bbox = density_grid->evalActiveVoxelBoundingBox();
    if (bbox.empty()) {
        MI_WARN("OpenVDBLoader: Density grid is empty.");
        file.close();
        return nullptr;
    }

    openvdb::Coord dim = bbox.dim(); // dim = max - min + 1
    MI_INFO("OpenVDBLoader: Native Grid Dim: {}x{}x{}", dim.x(), dim.y(), dim.z());

    // 如果原始分辨率超过 MaxDimension，我们被迫进行降采样，否则显存会爆炸。
    // 如果没有超过，我们就保持 1:1。
    uint32_t max_dim_actual = std::max({dim.x(), dim.y(), dim.z()});
    float scale_factor = 1.0f;

    if (max_dim_actual > options.MaxDimension) {
        scale_factor = (float)options.MaxDimension / max_dim_actual;
        MI_WARN("OpenVDBLoader: Grid too large, downscaling by factor {}", scale_factor);
    }

    // 计算最终纹理尺寸
    uint32_t width  = std::max(1u, (uint32_t)(dim.x() * scale_factor));
    uint32_t height = std::max(1u, (uint32_t)(dim.y() * scale_factor));
    uint32_t depth  = std::max(1u, (uint32_t)(dim.z() * scale_factor));

    // --- 3. 计算 World Space AABB ---
    openvdb::math::Transform::Ptr trans = density_grid->transformPtr();
    openvdb::Vec3d idx_min(bbox.min().x(), bbox.min().y(), bbox.min().z());
    openvdb::Vec3d idx_max(bbox.max().x() + 1, bbox.max().y() + 1, bbox.max().z() + 1);
    openvdb::Vec3d ws_min = trans->indexToWorld(idx_min);
    openvdb::Vec3d ws_max = trans->indexToWorld(idx_max);

    glm::vec3 vol_min(std::min(ws_min.x(), ws_max.x()), std::min(ws_min.y(), ws_max.y()), std::min(ws_min.z(), ws_max.z()));
    glm::vec3 vol_max(std::max(ws_min.x(), ws_max.x()), std::max(ws_min.y(), ws_max.y()), std::max(ws_min.z(), ws_max.z()));

    // TODO:修改模型本身以调整至合适尺寸
    // vol_min /= 50.f;
    // vol_max /= 50.f;

    // --- 4. 准备 Accessor (直接访问器) ---
    // 使用 ConstAccessor 是线程安全的，且比 GridSampler 更快
    auto density_acc = density_grid->getConstAccessor();

    // 准备颜色 Accessor
    openvdb::Vec3SGrid::ConstAccessor color_acc = openvdb::Vec3SGrid().getConstAccessor();
    bool has_color = false;

    if (color_grid_base) {
        auto color_grid = openvdb::gridPtrCast<openvdb::Vec3SGrid>(color_grid_base);
        color_acc = color_grid->getConstAccessor();
        has_color = true;
    }

    // --- 5. 填充数据 ---
    size_t total_voxels = (size_t)width * height * depth;
    std::vector<uint16_t> raw_data(total_voxels * 4);

    // 缓存 BBox 的起始坐标，以便在循环中加上偏移
    openvdb::Coord start_coord = bbox.min();

    // 切换到框架自带的 TaskGraph 处理每个切片，方便日后可能的统一调度
    TaskGraph::Get().ForEachBlockedRange((uint32_t)0, depth, [&](uint32_t block_begin, uint32_t block_end) {
        // 每个线程本地的 Coord 变量，避免反复构造
        openvdb::Coord i_coord;

        for (uint32_t z = block_begin; z != block_end; ++z) {
            for (uint32_t y = 0; y < height; ++y) {
                // 预计算 z 和 y 的偏移
                int offset_z = (scale_factor == 1.0f) ? z : (int)(z / scale_factor);
                int offset_y = (scale_factor == 1.0f) ? y : (int)(y / scale_factor);

                i_coord.setZ(start_coord.z() + offset_z);
                i_coord.setY(start_coord.y() + offset_y);

                for (uint32_t x = 0; x < width; ++x) {
                    int offset_x = (scale_factor == 1.0f) ? x : (int)(x / scale_factor);
                    i_coord.setX(start_coord.x() + offset_x);

                    // 1. 获取密度 (直接访问)
                    // getValue 会自动处理稀疏性：如果坐标处没有体素，返回 Background 值 (通常是0)
                    float density = density_acc.getValue(i_coord);
                    density *= options.DensityMultiplier;

                    // 2. 获取颜色 (直接访问)
                    // 因为保证了对齐，所以直接用同一个 i_coord 访问颜色网格
                    glm::vec3 color = options.DefaultColor;

                    if (has_color) {
                        openvdb::Vec3s c = color_acc.getValue(i_coord);
                        color = glm::vec3(c.x(), c.y(), c.z());
                    }

                    // 3. 打包 RGBA16F
                    uint32_t rg = glm::packHalf2x16({color.r, color.g});
                    uint32_t ba = glm::packHalf2x16({color.b, density});

                    size_t idx = ((size_t)z * height * width + (size_t)y * width + x) * 4;
                    raw_data[idx + 0] = (uint16_t)(rg & 0xFFFF);
                    raw_data[idx + 1] = (uint16_t)(rg >> 16);
                    raw_data[idx + 2] = (uint16_t)(ba & 0xFFFF);
                    raw_data[idx + 3] = (uint16_t)(ba >> 16);
                }
            }
        }
    });

    file.close();

    // --- 6. 创建资源 ---
    auto texture = VolumeTexture::Create(PixelFormatType::kR16G16B16A16_FLOAT, width, height, depth);
    texture->SetName(path.filename().string() + "_Tex");
    texture->SetBinary(std::span<uint8_t>(reinterpret_cast<uint8_t*>(raw_data.data()), raw_data.size() * sizeof(uint16_t)));

    auto volume_grid = VolumeGrid::Create();
    volume_grid->SetTexture(texture);
    volume_grid->SetAABB(AABB(vol_min, vol_max));

    return volume_grid;
}

MI_NAMESPACE_END