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

MACROMC_REGISTRY_NAMESPACE_END

#endif // MACROMC_REGISTRY_TYPES_H
