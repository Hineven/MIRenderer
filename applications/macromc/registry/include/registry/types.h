/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_REGISTRY_TYPES_H
#define MACROMC_REGISTRY_TYPES_H

#include "registry/common.h"
#include <cstdint>

MACROMC_REGISTRY_NAMESPACE_BEGIN

// Block ID type
using BlockId = uint16_t;

// Invalid and Air block IDs
static constexpr BlockId kInvalidBlockId = 0;
static constexpr BlockId kAirBlockId = 0;

// Maximum number of block types
static constexpr size_t kMaxBlockTypes = 65536;

// Block face enumeration (world-space face identification).
// Shared across registry/world/meshing; Pos/Neg pairs are interleaved so
// OppositeFace == face ^ 1.
enum class BlockFace : uint8_t {
    kPosX = 0,  // East  (+X)
    kNegX = 1,  // West  (-X)
    kPosY = 2,  // Up    (+Y)
    kNegY = 3,  // Down  (-Y)
    kPosZ = 4,  // South (+Z)
    kNegZ = 5,  // North (-Z)
    kCount = 6
};

MACROMC_REGISTRY_NAMESPACE_END

#endif // MACROMC_REGISTRY_TYPES_H
