/*
 * Created: 2025/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "micromc_world_types.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <string>

MI_NAMESPACE_BEGIN

// ==================== SubChunk Implementation ====================

SubChunk::SubChunk() {
    blocks_.fill(EMPTY_BLOCK_ID);
}

BlockID SubChunk::GetBlock(int x, int y, int z) const {
    if (x < 0 || x >= SUBCHUNK_SIZE || y < 0 || y >= SUBCHUNK_SIZE || z < 0 || z >= SUBCHUNK_SIZE) {
        return EMPTY_BLOCK_ID;
    }
    return blocks_[CoordToIndex(x, y, z)];
}

void SubChunk::SetBlock(int x, int y, int z, BlockID block_id) {
    if (x < 0 || x >= SUBCHUNK_SIZE || y < 0 || y >= SUBCHUNK_SIZE || z < 0 || z >= SUBCHUNK_SIZE) {
        return;
    }

    int index = CoordToIndex(x, y, z);
    BlockID old_block = blocks_[index];
    blocks_[index] = block_id;

    // 更新方块计数
    if (old_block == EMPTY_BLOCK_ID && block_id != EMPTY_BLOCK_ID) {
        block_count_++;
    } else if (old_block != EMPTY_BLOCK_ID && block_id == EMPTY_BLOCK_ID) {
        block_count_--;
    }
}

// ==================== BlockEntity Implementation ====================

BlockEntity::BlockEntity(const BlockCoord& position, BlockID block_id)
    : position_(position), block_id_(block_id) {
}

// ==================== Chunk Implementation ====================

Chunk::Chunk(const ChunkCoord& coord) : coord_(coord) {
    subchunks_.resize(MAX_SUBCHUNKS);
}

BlockID Chunk::GetBlock(int x, int y, int z) const {
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || y < 0) {
        return EMPTY_BLOCK_ID;
    }

    int subchunk_index = GetSubChunkIndex(y);
    if (subchunk_index >= MAX_SUBCHUNKS || !subchunks_[subchunk_index]) {
        return EMPTY_BLOCK_ID;
    }

    int local_x, local_y, local_z;
    GetLocalCoords(x, y, z, local_x, local_y, local_z);

    return subchunks_[subchunk_index]->GetBlock(local_x, local_y, local_z);
}

void Chunk::SetBlock(int x, int y, int z, BlockID block_id) {
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || y < 0) {
        return;
    }

    int subchunk_index = GetSubChunkIndex(y);
    if (subchunk_index >= MAX_SUBCHUNKS) {
        return;
    }

    // 如果要设置非空方块，确保SubChunk存在
    if (block_id != EMPTY_BLOCK_ID) {
        SubChunk* subchunk = GetOrCreateSubChunk(subchunk_index);
        int local_x, local_y, local_z;
        GetLocalCoords(x, y, z, local_x, local_y, local_z);
        subchunk->SetBlock(local_x, local_y, local_z, block_id);
    } else if (subchunks_[subchunk_index]) {
        // 设置空方块
        int local_x, local_y, local_z;
        GetLocalCoords(x, y, z, local_x, local_y, local_z);
        subchunks_[subchunk_index]->SetBlock(local_x, local_y, local_z, block_id);

        // 如果SubChunk变空，释放内存
        if (subchunks_[subchunk_index]->IsEmpty()) {
            subchunks_[subchunk_index].reset();
        }
    }
}

void Chunk::AddBlockEntity(TRef<BlockEntity> block_entity) {
    if (block_entity) {
        block_entities_[block_entity->GetPosition()] = block_entity;
    }
}

void Chunk::RemoveBlockEntity(const BlockCoord& position) {
    auto it = block_entities_.find(position);
    if (it != block_entities_.end()) {
        it->second->OnDestroy();
        block_entities_.erase(it);
    }
}

TRef<BlockEntity> Chunk::GetBlockEntity(const BlockCoord& position) const {
    auto it = block_entities_.find(position);
    return (it != block_entities_.end()) ? it->second : nullptr;
}

bool Chunk::IsEmpty() const {
    // 检查是否有任何非空的SubChunk
    for (const auto& subchunk : subchunks_) {
        if (subchunk && !subchunk->IsEmpty()) {
            return false;
        }
    }
    return block_entities_.empty();
}

void Chunk::Update(float delta_time) {
    // 更新所有BlockEntity
    for (auto& [pos, entity] : block_entities_) {
        entity->Update(delta_time);
    }
}

SubChunk* Chunk::GetOrCreateSubChunk(int subchunk_index) {
    if (subchunk_index < 0 || subchunk_index >= MAX_SUBCHUNKS) {
        return nullptr;
    }

    if (!subchunks_[subchunk_index]) {
        subchunks_[subchunk_index] = std::make_unique<SubChunk>();
    }

    return subchunks_[subchunk_index].get();
}

// ==================== WorldShell Implementation ====================

WorldShell::WorldShell() {
    transform_ = glm::mat4(1.0f);
}

void WorldShell::SetPosition(const glm::vec3& position) {
    // 保留当前的旋转和缩放，只更新位置
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(transform_, scale, rotation, translation, skew, perspective);

    transform_ = glm::translate(glm::mat4(1.0f), position) *
                 glm::mat4_cast(rotation) *
                 glm::scale(glm::mat4(1.0f), scale);
}

void WorldShell::SetRotation(const glm::quat& rotation) {
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat old_rotation;
    glm::decompose(transform_, scale, old_rotation, translation, skew, perspective);

    transform_ = glm::translate(glm::mat4(1.0f), translation) *
                 glm::mat4_cast(rotation) *
                 glm::scale(glm::mat4(1.0f), scale);
}

void WorldShell::SetScale(const glm::vec3& scale) {
    glm::vec3 old_scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(transform_, old_scale, rotation, translation, skew, perspective);

    transform_ = glm::translate(glm::mat4(1.0f), translation) *
                 glm::mat4_cast(rotation) *
                 glm::scale(glm::mat4(1.0f), scale);
}

glm::vec3 WorldShell::GetPosition() const {
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(transform_, scale, rotation, translation, skew, perspective);
    return translation;
}

glm::quat WorldShell::GetRotation() const {
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(transform_, scale, rotation, translation, skew, perspective);
    return rotation;
}

glm::vec3 WorldShell::GetScale() const {
    glm::vec3 scale, translation, skew;
    glm::vec4 perspective;
    glm::quat rotation;
    glm::decompose(transform_, scale, rotation, translation, skew, perspective);
    return scale;
}

void WorldShell::AddChunk(TRef<Chunk> chunk) {
    if (chunk) {
        chunks_[chunk->GetCoord()] = chunk;
    }
}

void WorldShell::RemoveChunk(const ChunkCoord& coord) {
    chunks_.erase(coord);
}

TRef<Chunk> WorldShell::GetChunk(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    return (it != chunks_.end()) ? it->second : nullptr;
}

BlockID WorldShell::GetBlock(const BlockCoord& world_pos) const {
    ChunkCoord chunk_coord = WorldToChunkCoord(world_pos);
    BlockCoord local_pos = WorldToChunkLocal(world_pos);

    TRef<Chunk> chunk = GetChunk(chunk_coord);
    if (!chunk) {
        return EMPTY_BLOCK_ID;
    }

    return chunk->GetBlock(local_pos.x, local_pos.y, local_pos.z);
}

void WorldShell::SetBlock(const BlockCoord& world_pos, BlockID block_id) {
    ChunkCoord chunk_coord = WorldToChunkCoord(world_pos);
    BlockCoord local_pos = WorldToChunkLocal(world_pos);

    TRef<Chunk> chunk = GetChunk(chunk_coord);
    if (!chunk && block_id != EMPTY_BLOCK_ID) {
        // 如果要设置非空方块，创建新的Chunk
        chunk = TRef<Chunk>(new Chunk(chunk_coord));
        AddChunk(chunk);
    }

    if (chunk) {
        chunk->SetBlock(local_pos.x, local_pos.y, local_pos.z, block_id);

        // 如果Chunk变空，考虑移除它
        if (chunk->IsEmpty()) {
            RemoveChunk(chunk_coord);
        }
    }
}

void WorldShell::Update(float delta_time) {
    for (auto& [coord, chunk] : chunks_) {
        chunk->Update(delta_time);
    }
}

ChunkCoord WorldShell::WorldToChunkCoord(const BlockCoord& world_pos) {
    return ChunkCoord(
        world_pos.x < 0 ? (world_pos.x - CHUNK_SIZE + 1) / CHUNK_SIZE : world_pos.x / CHUNK_SIZE,
        world_pos.z < 0 ? (world_pos.z - CHUNK_SIZE + 1) / CHUNK_SIZE : world_pos.z / CHUNK_SIZE
    );
}

BlockCoord WorldShell::WorldToChunkLocal(const BlockCoord& world_pos) {
    ChunkCoord chunk_coord = WorldToChunkCoord(world_pos);
    return BlockCoord(
        world_pos.x - chunk_coord.x * CHUNK_SIZE,
        world_pos.y,
        world_pos.z - chunk_coord.y * CHUNK_SIZE
    );
}

// ==================== Dimension Implementation ====================

Dimension::Dimension(const std::string& name) : name_(name) {
}

void Dimension::AddWorldShell(TRef<WorldShell> world_shell) {
    if (world_shell) {
        world_shells_.push_back(world_shell);
    }
}

void Dimension::RemoveWorldShell(TRef<WorldShell> world_shell) {
    auto it = std::find(world_shells_.begin(), world_shells_.end(), world_shell);
    if (it != world_shells_.end()) {
        world_shells_.erase(it);
    }
}

void Dimension::Update(float delta_time) {
    for (auto& world_shell : world_shells_) {
        world_shell->Update(delta_time);
    }
}

Dimension::BlockQueryResult Dimension::QueryBlock(const BlockCoord& world_pos) const {
    BlockQueryResult result;

    for (const auto& world_shell : world_shells_) {
        ChunkCoord chunk_coord = WorldShell::WorldToChunkCoord(world_pos);
        TRef<Chunk> chunk = world_shell->GetChunk(chunk_coord);

        if (chunk) {
            BlockCoord local_pos = WorldShell::WorldToChunkLocal(world_pos);
            BlockID block_id = chunk->GetBlock(local_pos.x, local_pos.y, local_pos.z);

            if (block_id != EMPTY_BLOCK_ID) {
                result.world_shell = world_shell;
                result.chunk = chunk;
                result.block_id = block_id;
                result.found = true;
                return result;
            }
        }
    }

    return result;
}

MI_NAMESPACE_END
