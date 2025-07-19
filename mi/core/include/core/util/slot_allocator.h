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
};


MI_NAMESPACE_END

#endif //SLOT_ALLOCATOR_H
