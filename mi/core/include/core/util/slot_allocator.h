/*
 * Created: 2025/7/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef SLOT_ALLOCATOR_H
#define SLOT_ALLOCATOR_H
#include <stack>
#include <core/common.h>
MI_NAMESPACE_BEGIN

// A simple slot allocator that allows allocation and deallocation of slot indices.
struct SlotAllocator {
    uint32_t max_num_slots_;
    // Unused slots. Initialized to max_num_slots_ elements upon construction.
    std::stack<uint32_t> free_slots_;

    FORCEINLINE SlotAllocator(uint32_t max_num_slots)
        : max_num_slots_(max_num_slots) {
        // Initialize the free slots stack with all slots
        for (int i = (int)max_num_slots - 1; i >= 0; i--) {
            free_slots_.push(i);
        }
    }

    FORCEINLINE void Reset() {
        // Reset the free slots stack to contain all slots again
        while (!free_slots_.empty()) {
            free_slots_.pop();
        }
        for (int i = (int)max_num_slots_ - 1; i >= 0; i--) {
            free_slots_.push(i);
        }
    }

    FORCEINLINE uint32_t AllocateSlot() {
        if (free_slots_.empty()) {
            return UINT32_MAX; // No free slots available
        }
        uint32_t slot = free_slots_.top();
        free_slots_.pop();
        return slot;
    }
    FORCEINLINE bool FreeSlot(uint32_t slot) {
        if (slot < max_num_slots_) {
            free_slots_.push(slot);
            return true;
        }
        return false;
    }

    FORCEINLINE uint32_t GetMaxNumSlots() const {
        return max_num_slots_;
    }

    FORCEINLINE bool NoAllocationActive() const {
        return free_slots_.size() == max_num_slots_;
    }
};

// A simple slot allocator that allows allocation and deallocation of slot indices. It can extend its capacity.
struct ExtendableSlotAllocator {
    uint32_t next_slot_index_ = 0;
    std::stack<uint32_t> free_slots_;

    FORCEINLINE ExtendableSlotAllocator() = default;

    FORCEINLINE uint32_t AllocateSlot() {
        if (!free_slots_.empty()) {
            uint32_t slot = free_slots_.top();
            free_slots_.pop();
            return slot;
        }
        return next_slot_index_++;
    }

    FORCEINLINE bool FreeSlot(uint32_t slot) {
        free_slots_.push(slot);
        return true;
    }

    FORCEINLINE uint32_t GetMaxNumSlots() const {
        return next_slot_index_;
    }

    FORCEINLINE bool NoAllocationActive() const {
        return free_slots_.size() == next_slot_index_;
    }
};


MI_NAMESPACE_END

#endif //SLOT_ALLOCATOR_H
