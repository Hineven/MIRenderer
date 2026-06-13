/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_REGISTRY_BLOCK_DEF_H
#define MACROMC_REGISTRY_BLOCK_DEF_H

#include "registry/common.h"
#include "registry/types.h"
#include <string>

MACROMC_REGISTRY_NAMESPACE_BEGIN

// Block definition: static properties of a block type
struct BlockDefinition {
    BlockId id = kInvalidBlockId;
    std::string name;           // e.g. "macromc:stone"
    bool solid = true;          // Whether the block is solid
    bool transparent = false;   // Whether the block is transparent (for rendering culling)
    bool render = true;         // Whether the block should be rendered
    
    // Phase 1: Simple single texture index
    uint16_t texture_index = 0; // Texture atlas index
    
    // Per-face properties (for greedy meshing and face culling)
    // Default: all faces match the block-level solid/render properties
    bool face_solid[static_cast<int>(BlockFace::kCount)] = {true, true, true, true, true, true};
    bool face_render[static_cast<int>(BlockFace::kCount)] = {true, true, true, true, true, true};
    
    // Convenience: check if a specific face is solid
    bool IsFaceSolid(BlockFace face) const {
        return face_solid[static_cast<int>(face)];
    }
    
    // Convenience: check if a specific face should be rendered
    bool IsFaceRendered(BlockFace face) const {
        return face_render[static_cast<int>(face)];
    }
    
    // Set all faces to solid (cube-like block)
    void SetAllFacesSolid() {
        for (int i = 0; i < static_cast<int>(BlockFace::kCount); ++i) {
            face_solid[i] = true;
            face_render[i] = true;
        }
    }
    
    // Future extensions:
    // - Multi-face textures (top, side, bottom)
    // - Model definition
    // - State parser
};

// Builtin block IDs (Phase 1)
namespace BuiltinBlocks {
    static constexpr BlockId kAirId = 0;
    static constexpr BlockId kStoneId = 1;
    static constexpr BlockId kDirtId = 2;
    static constexpr BlockId kGrassId = 3;
    static constexpr BlockId kBedrockId = 4;
    static constexpr BlockId kSandId = 5;
    static constexpr BlockId kWaterId = 6;
}

MACROMC_REGISTRY_NAMESPACE_END

#endif // MACROMC_REGISTRY_BLOCK_DEF_H
