/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_COMMON_H
#define MIRENDERER_CORE_COMMON_H


#include "core/platform.h"
#define MI_NAMESPACE mi

#define MI_NAMESPACE_BEGIN namespace MI_NAMESPACE {
#define MI_NAMESPACE_END }

#define MI_APPLICATION_VERSION_MAJOR 0
#define MI_APPLICATION_VERSION_MINOR 1
#define MI_ENGINE_NAME "MiRenderer"
#define MI_ENGINE_VERSION_MAJOR 0
#define MI_ENGINE_VERSION_MINOR 1

#include <cstdint>

#ifdef MI_COMPILER_MSVC
#define ALIGN(x) __declspec(align(x))
#endif
#ifdef MI_COMPILER_GNU
#define ALIGN(x) __attribute__((aligned(x)))
#endif
#ifndef ALIGN
#error "No alignment attribute for the current compiler"
#endif

#define ALIGNAS(x) alignas(x)

// TODO
#define CHECK_THREAD(thread)

#endif //MIRENDERER_CORE_COMMON_H
