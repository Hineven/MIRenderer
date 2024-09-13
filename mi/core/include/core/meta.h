/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_META_H
#define MIRENDERER_CORE_META_H

#include <type_traits>
#include "core/common.h"

MI_NAMESPACE_BEGIN

template<typename, typename = void>
struct TIsTypeComplete : std::false_type {};

template<typename T>
struct TIsTypeComplete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

template<typename T>
concept CMemTrivial = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// Array of unknown bound
template<typename T>
concept CAOUB = std::is_array_v<T> && std::extent_v<T> == 0;

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_META_H
