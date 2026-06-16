/*
 * Created: 2026/06/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

// =============================================================================
// macromc.h — MacroMC public helper aggregate header.
//
// External / application code that builds on MIRenderer's `mi` core frequently
// needs the same handful of helpers (reference counting, smart pointers, copy /
// move guards, platform macros). Instead of re-declaring or re-including the
// individual `core/*.h` headers from every translation unit, this header is the
// single place that:
//   1. pulls in the relevant `mi` headers, and
//   2. re-exports the most commonly used names into the `macromc` namespace via
//      `using` declarations.
//
// A nice side effect: `macromc::RefCounted` (a plain alias of `mi::RefCounted<>`,
// i.e. with default template arguments already applied) sidesteps the MSVC
// "use of class template requires template argument list" foot-gun that
// `mi::RefCounted` (bare, no `<>`) triggers.
//
// Add new re-exports here as the need arises; keep this header lean.
// =============================================================================

#ifndef MACROMC_MACROMC_H
#define MACROMC_MACROMC_H

#include "core/base.h"        // NonCopyable, NonMovable
#include "core/platform.h"    // FORCEINLINE, platform init, etc.
#include "core/refcounted.h"  // RefCounted<>, TRef<>, CReferenceCounted

// =============================================================================
// MacroMC namespace macros.
//
// These used to live in each module's own common.h. Centralizing them here lets
// every macromc module share one definition.
// =============================================================================
#define MACROMC_NAMESPACE macromc

#define MACROMC_NAMESPACE_BEGIN namespace macromc {
#define MACROMC_NAMESPACE_END }

MACROMC_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// Re-exported mi helpers.
//
// Prefer the `macromc::` aliases below in macromc code. They carry the default
// template arguments where relevant, so callers never write a bare template
// name by mistake.
// ---------------------------------------------------------------------------

// Reference-counted base. Defaults: thread-safe, no weak refs.
// Use `macromc::RefCounted` instead of `mi::RefCounted<>` to avoid the bare
// template-name pitfall on MSVC.
using RefCounted = mi::RefCounted<>;

// Smart pointer to a reference-counted object.
template <typename T>
using TRef = mi::TRef<T>;

// Copy / move guards.
using NonCopyable = mi::NonCopyable;
using NonMovable = mi::NonMovable;

MACROMC_NAMESPACE_END

#endif // MACROMC_MACROMC_H
