/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_SHELL_DATA_H
#define MACROMC_WORLD_SHELL_DATA_H

#include "world/common.h"
#include "world/types.h"
#include "world/chunk_data.h"
#include "core/refcounted.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <unordered_map>

MACROMC_WORLD_NAMESPACE_BEGIN

// Hash function for ChunkCoord
struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const;
};

// WorldShellData: Shell data container
// A Shell is a collection of Chunks that can be transformed together
class WorldShellData : public mi::RefCounted {
public:
    WorldShellData(ShellId id, ShellCategory category);
    ~WorldShellData() override;
    
    // Non-copyable
    WorldShellData(const WorldShellData&) = delete;
    WorldShellData& operator=(const WorldShellData&) = delete;
    
    // Basic properties
    ShellId GetId() const { return id_; }
    ShellCategory GetCategory() const { return category_; }
    
    // Transform (Phase 1: fixed to identity)
    const glm::mat4& GetTransform() const { return transform_; }
    void SetTransform(const glm::mat4& transform);
    
    // Chunk management
    mi::TRef<ChunkData> GetChunk(const ChunkCoord& coord);
    const mi::TRef<ChunkData> GetChunk(const ChunkCoord& coord) const;
    void SetChunk(const ChunkCoord& coord, mi::TRef<ChunkData> chunk);
    bool HasChunk(const ChunkCoord& coord) const;
    void RemoveChunk(const ChunkCoord& coord);
    
    // Iteration
    const auto& GetAllChunks() const { return chunks_; }
    size_t GetChunkCount() const { return chunks_.size(); }
    
private:
    ShellId id_;
    ShellCategory category_;
    glm::mat4 transform_{1.0f};  // Identity matrix
    
    std::unordered_map<ChunkCoord, mi::TRef<ChunkData>, ChunkCoordHash> chunks_;
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_SHELL_DATA_H
