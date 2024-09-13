/*
 * Created: 2024/7/10
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_ROUNDING_H
#define MIRENDERER_ROUNDING_H
#include <type_traits>
#include "core/common.h"
MI_NAMESPACE_BEGIN

namespace detail {

    template<typename T, typename T2, typename = std::enable_if<std::is_integral_v<T> && std::is_convertible_v<T2, T>>>
    struct RoundUpImpl {
        inline static T RoundUp(T value, T2 multiple) {
            return ((value + static_cast<T>(multiple) - 1) / static_cast<T>(multiple)) * static_cast<T>(multiple);
        }
    };
}

template<typename T, typename T2>
T RoundUp(T value, T2 multiple) {
    return detail::RoundUpImpl<T, T2>::RoundUp(value, multiple);
}

MI_NAMESPACE_END

#endif //MIRENDERER_ROUNDING_H
