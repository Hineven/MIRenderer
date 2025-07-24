/*
 * Created: 2025/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MICROMC_WORLD_TYPES_H
#define MICROMC_WORLD_TYPES_H

#include <vector>
#include <unordered_map>
#include <memory>
#include <array>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/refcounted.h"
#include "core/base.h"
#include "core/common.h"

MI_NAMESPACE_BEGIN

// 前向声明
class BlockEntity;
class Chunk;
class WorldShell;
class Dimension;

// 16bit 方块ID类型
using BlockID = uint16_t;

// 坐标类型定义
using ChunkCoord = glm::ivec2;  // 区块坐标 (x, z)
using BlockCoord = glm::ivec3;  // 方块坐标 (x, y, z)
using SubChunkIndex = int;      // SubChunk索引 (0-255)

// 为坐标类型提供hash函数
struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& v) const {
        return std::hash<int>()(v.x) ^ (std::hash<int>()(v.y) << 1);
    }
};

struct BlockCoordHash {
    size_t operator()(const BlockCoord& v) const {
        return std::hash<int>()(v.x) ^ (std::hash<int>()(v.y) << 1) ^ (std::hash<int>()(v.z) << 2);
    }
};

// 常量定义
constexpr int CHUNK_SIZE = 16;              // 区块大小 16x16
constexpr int SUBCHUNK_SIZE = 16;           // SubChunk大小 16x16x16
constexpr int MAX_SUBCHUNKS = 256;          // 最大SubChunk数量
constexpr BlockID EMPTY_BLOCK_ID = 0;       // 空方块ID

/**
 * SubChunk - 存储16x16x16的方块数据
 * 以稀疏方式存储，仅在非空时分配
 */
class SubChunk : public NonCopyable, public NonMovable {
public:
    SubChunk();
    ~SubChunk() = default;

    // 获取/设置方块ID
    BlockID GetBlock(int x, int y, int z) const;
    void SetBlock(int x, int y, int z, BlockID block_id);

    // 检查是否为空
    bool IsEmpty() const { return block_count_ == 0; }

    // 获取非空方块数量
    int GetBlockCount() const { return block_count_; }

private:
    std::array<BlockID, SUBCHUNK_SIZE * SUBCHUNK_SIZE * SUBCHUNK_SIZE> blocks_;
    int block_count_ = 0;  // 非空方块计数

    // 坐标转换为索引
    static int CoordToIndex(int x, int y, int z) {
        return (y * SUBCHUNK_SIZE + z) * SUBCHUNK_SIZE + x;
    }
};

/**
 * BlockEntity - 拥有特殊数据的方块基类
 */
class BlockEntity : public RefCounted<>, public NonCopyable, public NonMovable {
public:
    BlockEntity(const BlockCoord& position, BlockID block_id);
    virtual ~BlockEntity() = default;

    // 获取位置和方块ID
    const BlockCoord& GetPosition() const { return position_; }
    BlockID GetBlockID() const { return block_id_; }

    // 虚函数供子类重写
    virtual void Update(float delta_time) {}
    virtual void OnDestroy() {}

protected:
    BlockCoord position_;
    BlockID block_id_;
};

/**
 * Chunk - 16x16大小的区块
 * 可以划分为16x16x16的SubChunk，以稀疏方式存储
 */
class Chunk : public RefCounted<>, public NonCopyable, public NonMovable {
public:
    Chunk(const ChunkCoord& coord);
    ~Chunk() = default;

    // 获取区块坐标
    const ChunkCoord& GetCoord() const { return coord_; }

    // 方块操作
    BlockID GetBlock(int x, int y, int z) const;
    void SetBlock(int x, int y, int z, BlockID block_id);

    // BlockEntity 管理
    void AddBlockEntity(TRef<BlockEntity> block_entity);
    void RemoveBlockEntity(const BlockCoord& position);
    TRef<BlockEntity> GetBlockEntity(const BlockCoord& position) const;

    // 获取所有SubChunk（用于渲染等）
    const std::vector<std::unique_ptr<SubChunk>>& GetSubChunks() const { return subchunks_; }

    // 检查区块是否为空
    bool IsEmpty() const;

    // 更新所有BlockEntity
    void Update(float delta_time);

private:
    ChunkCoord coord_;
    std::vector<std::unique_ptr<SubChunk>> subchunks_;  // 最多256个SubChunk
    std::unordered_map<BlockCoord, TRef<BlockEntity>, BlockCoordHash> block_entities_;

    // 获取或创建SubChunk
    SubChunk* GetOrCreateSubChunk(int subchunk_index);

    // 计算SubChunk索引
    static int GetSubChunkIndex(int y) {
        return y / SUBCHUNK_SIZE;
    }

    // 计算SubChunk内的局部坐标
    static void GetLocalCoords(int x, int y, int z, int& local_x, int& local_y, int& local_z) {
        local_x = x % SUBCHUNK_SIZE;
        local_y = y % SUBCHUNK_SIZE;
        local_z = z % SUBCHUNK_SIZE;
    }
};

/**
 * WorldShell - Chunk的集合体
 * 拥有独立的Transform，将来可以作为刚体物理结构
 */
class WorldShell : public RefCounted<>, public NonCopyable, public NonMovable {
public:
    WorldShell();
    ~WorldShell() = default;

    // Transform 管理
    const glm::mat4& GetTransform() const { return transform_; }
    void SetTransform(const glm::mat4& transform) { transform_ = transform; }

    // 位置、旋转、缩放操作
    void SetPosition(const glm::vec3& position);
    void SetRotation(const glm::quat& rotation);
    void SetScale(const glm::vec3& scale);

    glm::vec3 GetPosition() const;
    glm::quat GetRotation() const;
    glm::vec3 GetScale() const;

    // Chunk 管理
    void AddChunk(TRef<Chunk> chunk);
    void RemoveChunk(const ChunkCoord& coord);
    TRef<Chunk> GetChunk(const ChunkCoord& coord) const;

    // 获取所有Chunk
    const std::unordered_map<ChunkCoord, TRef<Chunk>, ChunkCoordHash>& GetChunks() const {
        return chunks_;
    }

    // 方块操作（会自动处理跨Chunk的情况）
    BlockID GetBlock(const BlockCoord& world_pos) const;
    void SetBlock(const BlockCoord& world_pos, BlockID block_id);

    // 更新所有Chunk
    void Update(float delta_time);

    // 将世界坐标转换为区块坐标和区块内坐标（设为public供Dimension使用）
    static ChunkCoord WorldToChunkCoord(const BlockCoord& world_pos);
    static BlockCoord WorldToChunkLocal(const BlockCoord& world_pos);

private:
    glm::mat4 transform_ = glm::mat4(1.0f);
    std::unordered_map<ChunkCoord, TRef<Chunk>, ChunkCoordHash> chunks_;
};

/**
 * Dimension - 代表一整个维度
 * 拥有多个WorldShell
 */
class Dimension : public RefCounted<>, public NonCopyable, public NonMovable {
public:
    Dimension(const std::string& name);
    ~Dimension() = default;

    // 获取维度名称
    const std::string& GetName() const { return name_; }

    // WorldShell 管理
    void AddWorldShell(TRef<WorldShell> world_shell);
    void RemoveWorldShell(TRef<WorldShell> world_shell);

    // 获取所有WorldShell
    const std::vector<TRef<WorldShell>>& GetWorldShells() const { return world_shells_; }

    // 更新所有WorldShell
    void Update(float delta_time);

    // 全局方块查找（在所有WorldShell中搜索）
    struct BlockQueryResult {
        TRef<WorldShell> world_shell;
        TRef<Chunk> chunk;
        BlockID block_id;
        bool found = false;
    };

    BlockQueryResult QueryBlock(const BlockCoord& world_pos) const;

private:
    std::string name_;
    std::vector<TRef<WorldShell>> world_shells_;
};

MI_NAMESPACE_END

// 为坐标类型添加hash支持
namespace std {
    template<>
    struct hash<glm::ivec2> {
        size_t operator()(const glm::ivec2& v) const {
            return hash<int>()(v.x) ^ (hash<int>()(v.y) << 1);
        }
    };

    template<>
    struct hash<glm::ivec3> {
        size_t operator()(const glm::ivec3& v) const {
            return hash<int>()(v.x) ^ (hash<int>()(v.y) << 1) ^ (hash<int>()(v.z) << 2);
        }
    };
}

#endif // MICROMC_WORLD_TYPES_H
