/*
 * Created: 2024/9/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VMA_OVERRIDES_H
#define MI_VMA_OVERRIDES_H

#include <core/infra.h>
// Replace the default assert function with mi_assert
#define VMA_ASSERT(expr) mi_assert(expr, "VMA_ASSERT failed")

#endif //MI_VMA_OVERRIDES_H
