/*
 * Created: 2025/3/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef BYTE_STRIDED_SPAN_H
#define BYTE_STRIDED_SPAN_H

#include <iterator>
#include "core/common.h"

MI_NAMESPACE_BEGIN

template<typename T>
class byte_strided_span {
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T*;
        using reference = T&;

        iterator() : ptr_(nullptr), stride_(0) {}
        iterator(char* ptr, std::size_t stride) : ptr_(ptr), stride_(stride) {}

        reference operator*() const { return *reinterpret_cast<T*>(ptr_); }
        pointer operator->() const { return reinterpret_cast<T*>(ptr_); }

        iterator& operator++() { ptr_ += stride_; return *this; }
        iterator operator++(int) { iterator tmp = *this; ptr_ += stride_; return tmp; }
        iterator& operator--() { ptr_ -= stride_; return *this; }
        iterator operator--(int) { iterator tmp = *this; ptr_ -= stride_; return tmp; }

        iterator& operator+=(difference_type n) { ptr_ += n * stride_; return *this; }
        iterator& operator-=(difference_type n) { ptr_ -= n * stride_; return *this; }
        iterator operator+(difference_type n) const { return iterator(ptr_ + n * stride_, stride_); }
        iterator operator-(difference_type n) const { return iterator(ptr_ - n * stride_, stride_); }

        difference_type operator-(const iterator& other) const { return (ptr_ - other.ptr_) / stride_; }

        bool operator==(const iterator& other) const { return ptr_ == other.ptr_; }
        bool operator!=(const iterator& other) const { return ptr_ != other.ptr_; }
        bool operator<(const iterator& other) const { return ptr_ < other.ptr_; }
        bool operator>(const iterator& other) const { return ptr_ > other.ptr_; }
        bool operator<=(const iterator& other) const { return ptr_ <= other.ptr_; }
        bool operator>=(const iterator& other) const { return ptr_ >= other.ptr_; }

    private:
        char* ptr_;
        std::size_t stride_;
    };

    byte_strided_span() : data_(nullptr), size_(0), stride_(0) {}

    byte_strided_span(T* data, std::size_t size, std::size_t byte_stride)
        : data_(reinterpret_cast<char*>(data)), size_(size), stride_(byte_stride) {}

    T& operator[](std::size_t index) {
        return *reinterpret_cast<T*>(data_ + index * stride_);
    }

    const T& operator[](std::size_t index) const {
        return *reinterpret_cast<const T*>(data_ + index * stride_);
    }

    std::size_t size() const {
        return size_;
    }

    iterator begin() {
        return iterator(data_, stride_);
    }

    const iterator begin() const {
        return iterator(data_, stride_);
    }

    iterator end() {
        return iterator(data_ + size_ * stride_, stride_);
    }

    const iterator end() const {
        return iterator(data_ + size_ * stride_, stride_);
    }

private:
    char* data_;
    std::size_t size_;
    std::size_t stride_;
};

MI_NAMESPACE_END

#endif //BYTE_STRIDED_SPAN_H
