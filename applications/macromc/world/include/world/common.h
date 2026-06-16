/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_COMMON_H
#define MACROMC_WORLD_COMMON_H

#include "macromc/macromc.h"

// MacroMC uses a single flat `macromc::` namespace for now. Each module keeps
// its own BEGIN/END alias so existing call sites compile unchanged, but they
// all resolve to the same namespace.
#define MACROMC_WORLD_NAMESPACE MACROMC_NAMESPACE

#define MACROMC_WORLD_NAMESPACE_BEGIN MACROMC_NAMESPACE_BEGIN
#define MACROMC_WORLD_NAMESPACE_END MACROMC_NAMESPACE_END

#endif // MACROMC_WORLD_COMMON_H
