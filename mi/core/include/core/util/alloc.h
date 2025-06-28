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
class TOneTimeLinearAllocator : public NonCopyable, public NonMovable {
public:
    TOneTimeLinearAllocator() {
        auto new_block = (Block*)operator new (sizeof(Block), std::align_val_t(Alignment));
        new_block->next = nullptr;
        head_ = new_block;
        current_ = head_;
    }
    ~TOneTimeLinearAllocator() {
        Release();
    }

    void * Allocate (size_t size) {
        if(size == 0) return nullptr; // Silently ignore zero size allocation
        size = RoundUp(size, Alignment);
        mi_assert(size <= BlockSize, "Allocation size exceeds block size");
        if(current_offset_ + size > BlockSize) {
            auto new_block = reinterpret_cast<Block*>(operator new (sizeof(Block), std::align_val_t(Alignment)));
            new_block->next = nullptr;
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
            operator delete (block, std::align_val_t(Alignment));
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

// Fixed-size element allocator for a fixed number of elements of type T.
// bThreadSafe: If true, the allocator is thread-safe and uses a mutex for synchronization.
// bRawAllocation: If true, the allocator does not call constructors/destructors on T.
template<typename T, size_t ElementCount, size_t Alignment = 16, bool bThreadSafe = false, bool bRawAllocation = false>
class TFixedElementAllocator : public NonCopyable, public NonMovable {
public:
    TFixedElementAllocator() {
        // Initialize free list - all slots are initially free
        for (size_t i = 0; i < ElementCount - 1; ++i) {
            GetFreeSlot(i)->next_free = i + 1;
        }
        GetFreeSlot(ElementCount - 1)->next_free = kInvalidIndex;
        first_free_ = 0;
        allocated_count_ = 0;
    }

    ~TFixedElementAllocator() {
        // Destructor should only be called when all elements are freed
        // In debug mode, we can assert this
        assert(allocated_count_ == 0 && "Not all elements were freed before allocator destruction");
    }

    template<typename...InitArgs>
    T* Allocate(InitArgs&&...args) {
        if constexpr (bThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return AllocateInternal(std::forward<InitArgs>(args)...);
        } else {
            return AllocateInternal(std::forward<InitArgs>(args)...);
        }
    }

    void Free(T* ptr) {
        if (!ptr) return;

        if constexpr (bThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            FreeInternal(ptr);
        } else {
            FreeInternal(ptr);
        }
    }

    // Get the number of allocated elements
    size_t GetAllocatedCount() const {
        if constexpr (bThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return allocated_count_;
        } else {
            return allocated_count_;
        }
    }

    // Get the maximum number of elements this allocator can hold
    constexpr size_t GetCapacity() const {
        return ElementCount;
    }

    // Check if the allocator is full
    bool IsFull() const {
        if constexpr (bThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return allocated_count_ == ElementCount;
        } else {
            return allocated_count_ == ElementCount;
        }
    }

    // Check if a pointer belongs to this allocator
    bool OwnsPointer(const T* ptr) const {
        const std::byte* byte_ptr = reinterpret_cast<const std::byte*>(ptr);
        const std::byte* buffer_start = buffer_;
        const std::byte* buffer_end = buffer_ + sizeof(buffer_);
        return byte_ptr >= buffer_start && byte_ptr < buffer_end;
    }

private:
    static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

    // Ensure proper alignment
    static constexpr size_t kElementSize = (sizeof(T) + Alignment - 1) & ~(Alignment - 1);

    struct FreeSlot {
        size_t next_free;
    };

    // Ensure that the element size is greater than or equal to the size of FreeSlot
    static_assert(kElementSize >= sizeof(FreeSlot), "Element size must be at least as large as FreeSlot size");

    template<typename...InitArgs>
    T* AllocateInternal(InitArgs&&...args) {
        if (first_free_ == kInvalidIndex) {
            // No free slots available
            return nullptr;
        }

        // Get the first free slot
        size_t slot_index = first_free_;
        FreeSlot* free_slot = GetFreeSlot(slot_index);
        first_free_ = free_slot->next_free;

        // Get pointer to the element location
        T* element_ptr = GetElementPtr(slot_index);

        // Construct the element in place
        if constexpr (!bRawAllocation) {
            new (element_ptr) T(std::forward<InitArgs>(args)...);
        }

        ++allocated_count_;
        return element_ptr;
    }

    void FreeInternal(T* ptr) {
        // Verify the pointer belongs to this allocator
        assert(OwnsPointer(ptr) && "Pointer does not belong to this allocator");

        // Calculate the slot index
        size_t slot_index = GetSlotIndex(ptr);
        assert(slot_index < ElementCount && "Invalid slot index");


        if constexpr (!bRawAllocation) {
            // Call destructor
            ptr->~T();
        }

        // Add the slot back to the free list
        FreeSlot* free_slot = GetFreeSlot(slot_index);
        free_slot->next_free = first_free_;
        first_free_ = slot_index;

        --allocated_count_;
    }

    T* GetElementPtr(size_t index) {
        assert(index < ElementCount);
        return reinterpret_cast<T*>(buffer_ + index * kElementSize);
    }

    FreeSlot* GetFreeSlot(size_t index) {
        assert(index < ElementCount);
        return reinterpret_cast<FreeSlot*>(buffer_ + index * kElementSize);
    }

    size_t GetSlotIndex(const T* ptr) const {
        const std::byte* byte_ptr = reinterpret_cast<const std::byte*>(ptr);
        const std::byte* buffer_start = buffer_;
        size_t offset = byte_ptr - buffer_start;
        return offset / kElementSize;
    }

    // Buffer to hold all elements, properly aligned
    ALIGNAS(Alignment) std::byte buffer_[ElementCount * kElementSize];

    // Index of the first free slot (kInvalidIndex if no free slots)
    size_t first_free_ = 0;

    // Number of currently allocated elements
    size_t allocated_count_ = 0;

    // Mutex for thread safety (only used if bThreadSafe is true)
    mutable std::conditional_t<bThreadSafe, std::mutex, char> mutex_;
};

MI_NAMESPACE_END
#endif //MIRENDERER_UTIL_ALLOC_H
