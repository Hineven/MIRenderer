/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_BASE_H
#define MIRENDERER_CORE_BASE_H

#include "core/common.h"
MI_NAMESPACE_BEGIN

class NonCopyable {
public:
    NonCopyable() = default;

    NonCopyable(const NonCopyable& Rhs) = delete;
    NonCopyable& operator=(const NonCopyable& Rhs) = delete;
};

class NonMovable {
public:
    NonMovable() = default;

    NonMovable(NonMovable&& Rhs) = delete;
    NonMovable& operator=(NonMovable&& Rhs) = delete;
};

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_BASE_H
