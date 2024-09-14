/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_UTIL_ALLOC_H
#define MIRENDERER_UTIL_ALLOC_H

#include "core/common.h"
#include "core/meta.h"
#include "core/infra.h"
#include "core/rounding.h"

MI_NAMESPACE_BEGIN

template<size_t BlockSize = 4096, size_t Alignment = 16>
class TOneTimeLinearAllocator {
public:
    TOneTimeLinearAllocator() {
        Block * new_block = (Block*)GetInfra().Allocate(sizeof(Block));
        head_ = new_block;
        current_ = head_;
    }
    ~TOneTimeLinearAllocator() {
        Release();
    }

    void * Allocate (size_t size) {
        mi_assert(size <= BlockSize, "Allocation size exceeds block size");
        mi_assert(size > 0, "Allocation must not be empty");
        size = RoundUp(size, Alignment);
        if(current_offset_ + size > BlockSize) {
            auto new_block = reinterpret_cast<Block*>(GetInfra().Allocate(sizeof(Block), Alignment));
            current_->next = new_block;
            current_ = new_block;
            current_offset_ = 0;
        }
        auto last_offset = current_offset_;
        current_offset_ += size;
        return current_->data + last_offset;
    }

    void Reset () {
        current_ = head_;
        current_offset_ = 0;
    }

    void Release () {
        Block* block = head_;
        while (block) {
            Block* next = block->next;
            GetInfra().Free(block);
            block = next;
        }
        head_ = nullptr;
        current_ = nullptr;
        current_offset_ = 0;
    }

protected:
    struct Block {
        std::byte data[BlockSize];
        Block* next;
    };

    Block* head_ {};
    Block* current_ {};
    size_t current_offset_ {};
};

template<typename T, size_t ElementCount, size_t Alignment = 16>
class TFixedElementAllocator {
public:
    template<typename...InitArgs>
    T * Allocate (InitArgs...args) {
        assert(false);
    }
    void Free (T * ptr) {
        assert(false);
    }
};
MI_NAMESPACE_END
#endif //MIRENDERER_UTIL_ALLOC_H
