/*
 * Created: 2025/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MICROMC_H
#define MICROMC_H

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <glm/glm.hpp>

#include "core/refcounted.h"
#include "rdg/rdg_fwd.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_texture.h"

using namespace mi;

// MC世界格式中的方块数据
struct MCBlock {
    int x, y, z;
    std::string id;
};

// MC区块数据
struct MCChunk {
    int coord_x, coord_z;
    std::vector<MCBlock> blocks;
};

// MC世界数据
struct MCWorld {
    std::vector<MCChunk> chunks;
};

// 纹理图集中的UV映射信息
struct TextureUVMapping {
    float u_min, v_min, u_max, v_max;
};

// MicroMC区块类
class MicroMCChunk {
public:
    MicroMCChunk(int coord_x, int coord_z);
    ~MicroMCChunk();

    // 从方块数据更新几何体
    void UpdateGeometries(RendererScene * world, Material * block_material, const std::vector<MCBlock>& blocks,
                         const std::unordered_map<std::string, TextureUVMapping>& uv_mappings);

    // 获取静态网格
    StaticMesh * GetStaticMesh() const { return static_mesh_.Raw(); }

    // 获取几何体
    const std::vector<TRef<Geometry>> & GetGeometries() const { return geometries_; }

    // 获取区块坐标
    int GetCoordX() const { return coord_x_; }
    int GetCoordZ() const { return coord_z_; }

private:
    int coord_x_, coord_z_;
    TRef<StaticMesh> static_mesh_;
    std::vector<TRef<Geometry>> geometries_;

    // 生成立方体的顶点和索引
    void GenerateCubeGeometry(RendererScene * world, Material * block_material, const std::vector<MCBlock>& blocks,
                             const std::unordered_map<std::string, TextureUVMapping>& uv_mappings);

    // 检查方块是否存在
    bool IsBlockAt(const std::vector<MCBlock>& blocks, int x, int y, int z) const;

    // 添加立方体面
    void AddCubeFace(std::vector<DefaultStaticMeshVertex>& vertices,
                    std::vector<uint32_t>& indices,
                    const glm::vec3& position,
                    const glm::vec3& normal,
                    const glm::vec3& tangent,
                    const glm::vec3& bitangent,
                    const TextureUVMapping& uv_mapping);
};

// MicroMC世界类
class MicroMCWorld {
public:
    MicroMCWorld();
    ~MicroMCWorld();

    // 从目录加载MC世界
    bool LoadFromDirectory(const std::string& world_path, const std::string& resource_pack_path, RendererScene* scene);

private:
    std::vector<std::unique_ptr<MicroMCChunk>> chunks_;
    std::unordered_map<std::string, TextureUVMapping> block_uv_mappings_;
    TRef<Material> block_material_;
    TRef<Texture> atlas_texture_;

    // 加载世界数据
    bool LoadWorldData(const std::string& world_path, MCWorld& world_data);

    // 加载资源包并创建纹理图集
    bool LoadResourcePack(const std::string& resource_pack_path);

    // 创建纹理图集
    bool CreateTextureAtlas(const std::vector<std::string>& texture_paths,
                           const std::vector<std::string>& block_ids);

    // 创建方块材质
    void CreateBlockMaterial();
};

void MicroMCRenderFrame(RendererView * view_state, RDGResourcePool * pool) ;

#endif // MICROMC_H

