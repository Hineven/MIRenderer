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
    
    // Phase 1: Simple single texture index.
    // Deprecated: use face_textures[] instead. Kept for compatibility; the
    // mesher now reads GetFaceTexture() exclusively. Will be removed once all
    // registrations migrate.
    [[deprecated("use face_textures[] / SetUniformTexture / SetTopSideBottom")]]
    uint16_t texture_index = 0;

    // Per-face texture indices into the atlas. Indexed by BlockFace.
    // A block can have up to 6 different textures (one per face); most blocks
    // reuse the same tile on all faces (use SetUniformTexture) or use a
    // top/side/bottom split (use SetTopSideBottom).
    uint16_t face_textures[static_cast<int>(BlockFace::kCount)] = {0, 0, 0, 0, 0, 0};

    // Per-face properties (for greedy meshing and face culling)
    // Default: all faces match the block-level solid/render properties
    bool face_solid[static_cast<int>(BlockFace::kCount)] = {true, true, true, true, true, true};
    bool face_render[static_cast<int>(BlockFace::kCount)] = {true, true, true, true, true, true};

    // Convenience: texture index for a specific face.
    uint16_t GetFaceTexture(BlockFace face) const {
        return face_textures[static_cast<int>(face)];
    }

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

    // Texture helpers.
    // Set the same atlas tile on all 6 faces.
    void SetUniformTexture(uint16_t t) {
        for (int i = 0; i < static_cast<int>(BlockFace::kCount); ++i) {
            face_textures[i] = t;
        }
    }
    // Top/side/bottom split: +Y face gets `top`, -Y face gets `bottom`,
    // the four side faces (±X, ±Z) all get `side`. (MC-style.)
    void SetTopSideBottom(uint16_t top, uint16_t side, uint16_t bottom) {
        face_textures[static_cast<int>(BlockFace::kPosY)] = top;
        face_textures[static_cast<int>(BlockFace::kNegY)] = bottom;
        face_textures[static_cast<int>(BlockFace::kPosX)] = side;
        face_textures[static_cast<int>(BlockFace::kNegX)] = side;
        face_textures[static_cast<int>(BlockFace::kPosZ)] = side;
        face_textures[static_cast<int>(BlockFace::kNegZ)] = side;
    }

    // Future extensions:
    // - Model definition (AABB elements) for non-cube blocks
    // - State parser for connected blocks (fences, walls)
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
