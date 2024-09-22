/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CONALLOC_H
#define MIRENDERER_CONALLOC_H

#include "core/common.h"
#include "infra.h"

MI_NAMESPACE_BEGIN

// A default allocator for containers using infra allocator
template<typename T>
class InfraDefaultAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    InfraDefaultAllocator() noexcept = default;

    template<typename U>
    InfraDefaultAllocator(const InfraDefaultAllocator<U> & ) noexcept {} // NOLINT

    pointer allocate(size_type n, [[maybe_unused]] const void* hint = 0) {
        return static_cast<pointer>(GetInfra().Allocate(n * sizeof(T)));
    }

    void deallocate(pointer p, [[maybe_unused]] size_type n) {
        GetInfra().Free(p);
    }

    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        new(p) U(std::forward<Args>(args)...);
    }

    template<typename U>
    void destroy(U* p) {
        p->~U();
    }
};

template<typename T>
using IDA = std::allocator<T>;

template<typename T>
using IVector = std::vector<T, InfraDefaultAllocator<T>>;

using IString = std::basic_string<char, std::char_traits<char>, InfraDefaultAllocator<char>>;
using IWString = std::basic_string<wchar_t, std::char_traits<wchar_t>, InfraDefaultAllocator<wchar_t>>;

MI_NAMESPACE_END

#endif //MIRENDERER_CONALLOC_H
