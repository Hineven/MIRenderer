/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_PROFILER_COMMON_H
#define MACROMC_PROFILER_COMMON_H

#include "macromc/macromc.h"

// MacroMC uses a single flat `macromc::` namespace for now. Each module keeps
// its own BEGIN/END alias so existing call sites compile unchanged, but they
// all resolve to the same namespace.
#define MACROMC_PROFILER_NAMESPACE MACROMC_NAMESPACE

#define MACROMC_PROFILER_NAMESPACE_BEGIN MACROMC_NAMESPACE_BEGIN
#define MACROMC_PROFILER_NAMESPACE_END MACROMC_NAMESPACE_END

#endif // MACROMC_PROFILER_COMMON_H
