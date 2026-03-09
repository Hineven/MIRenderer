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
}

MACROMC_REGISTRY_NAMESPACE_END

#endif // MACROMC_REGISTRY_BLOCK_DEF_H
