/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/shell_data.h"

MACROMC_WORLD_NAMESPACE_BEGIN

// ChunkCoordHash implementation
size_t ChunkCoordHash::operator()(const ChunkCoord& c) const {
    // Simple hash combining x, y, z
    size_t h1 = std::hash<int>()(c.x);
    size_t h2 = std::hash<int>()(c.y);
    size_t h3 = std::hash<int>()(c.z);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
}

WorldShellData::WorldShellData(ShellId id, ShellCategory category)
    : id_(id), category_(category) {
}

WorldShellData::~WorldShellData() = default;

void WorldShellData::SetTransform(const glm::mat4& transform) {
    transform_ = transform;
}

mi::TRef<ChunkData> WorldShellData::GetChunk(const ChunkCoord& coord) {
    auto it = chunks_.find(coord);
    if (it != chunks_.end()) {
        return it->second;
    }
    return nullptr;
}

const mi::TRef<ChunkData> WorldShellData::GetChunk(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    if (it != chunks_.end()) {
        return it->second;
    }
    return nullptr;
}

void WorldShellData::SetChunk(const ChunkCoord& coord, mi::TRef<ChunkData> chunk) {
    chunks_[coord] = std::move(chunk);
}

bool WorldShellData::HasChunk(const ChunkCoord& coord) const {
    return chunks_.find(coord) != chunks_.end();
}

void WorldShellData::RemoveChunk(const ChunkCoord& coord) {
    chunks_.erase(coord);
}

MACROMC_WORLD_NAMESPACE_END
