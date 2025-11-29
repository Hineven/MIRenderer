/*
 * Created: 2025/7/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef SEGMENT_ALLOCATOR_H
#define SEGMENT_ALLOCATOR_H

#include <set>
#include <ranges>
#include <core/common.h>
#include <core/infra.h>
MI_NAMESPACE_BEGIN

// A simple segment allocator that allocates/frees non-overlapping segments over a fixed range of indices.
class SimpleSegmentAllocator {
    size_t max_num_elements_;
    size_t alignment;
    struct Segment {
        mutable size_t start_index;
        mutable size_t end_index; // Exclusive
        FORCEINLINE bool operator < (const Segment & b) const {
            return start_index < b.start_index;
        }
    };
    std::set<Segment> free_segments_;
public:
    FORCEINLINE SimpleSegmentAllocator (size_t max_num_elements, size_t alignment = 1)
        : max_num_elements_(max_num_elements), alignment(alignment) {
        mi_assert(max_num_elements > 0, "Max number of elements must be greater than 0.");
        free_segments_.emplace(0, max_num_elements_);
    }
    FORCEINLINE size_t Allocate(size_t num_elements) {
        num_elements = (num_elements + alignment - 1) / alignment * alignment; // Align the number of elements to the alignment
        // Simply iterate through all free segments and find the first one that can fit the allocation.
        Segment * found = {};
        for (auto & segment : free_segments_) {
            if (segment.end_index >= segment.start_index + num_elements) {
                // Found a segment that can fit the allocation
                found = const_cast<Segment*>(&segment);
                break;
            }
        }
        if (!found) {
            return SIZE_MAX; // No free segment found
        }
        size_t start_index = (found->start_index + alignment - 1) / alignment * alignment;
        size_t end_index = start_index + num_elements;
        if (found->end_index != end_index) {
            // If there is space after the allocated segment, add it back to the free segments
            *found = {end_index, found->end_index};
        } else {
            free_segments_.erase(*found);
        }
        return start_index;
    }
    // Expand the heap to at least size elements.
    FORCEINLINE void ExpandTo (size_t size) {
        if (size <= max_num_elements_) return;
        if (!free_segments_.empty()) {
            auto last_segment = std::prev(free_segments_.end());
            if (last_segment->end_index == max_num_elements_) {
                // Extend the last free segment
                auto start = last_segment->start_index;
                free_segments_.erase(last_segment);
                free_segments_.emplace(start, size);
            } else {
                // Add a new free segment
                free_segments_.emplace(max_num_elements_, size);
            }
        } else {
            // No free segments available, just add a new one
            free_segments_.emplace(max_num_elements_, size);
        }
    }
    FORCEINLINE void Free(size_t start_index, size_t num_elements) {
        num_elements = (num_elements + alignment - 1) / alignment * alignment; // Align the number of elements to the alignment
        size_t end_index = start_index + num_elements;
        mi_assert(start_index < max_num_elements_ && end_index <= max_num_elements_, "Invalid segment range.");
        // Find the segment that contains the start index
        auto it = free_segments_.lower_bound(Segment{start_index, 0});
        bool merged = false;
        if (it != free_segments_.end()) {
            // There is a segment that starts after the start index
            // Check if we can merge with the successive segment
            mi_assert(end_index <= it->start_index, "Cannot free segment that overlaps with an existing free segment.");
            if (end_index == it->start_index) {
                // Merge case
                it->start_index = start_index;
                merged = true;
            }
        }
        if (it != free_segments_.begin()) {
            // Check if we can merge with the previous segment
            auto prev_it = std::prev(it);
            if (prev_it->end_index == start_index) {
                // Merge case
                if (merged) {
                    // If we already merged with the successor, we need to erase the previous segment
                    // and update the current segment to the merged one
                    it->start_index = prev_it->start_index;
                    free_segments_.erase(prev_it);
                } else {
                    // Just merge with the previous segment
                    prev_it->end_index = end_index;
                    merged = true;
                }
            }
        }
        // If we didn't merge, we need to add a new segment
        if (!merged) {
            free_segments_.insert(it, {start_index, end_index});
        }
    }
    FORCEINLINE size_t GetFreeSegmentCount() const {
        return free_segments_.size();
    }
    FORCEINLINE size_t GetFreeSegmentSize(size_t index) const {
        mi_assert(index < free_segments_.size(), "Index out of bounds.");
        auto it = std::next(free_segments_.begin(), index);
        return it->end_index - it->start_index;
    }
    // Get the byte offset of the highest allocated bit + 1
    FORCEINLINE size_t GetMaxAllocationEndOffset () const {
        auto last_free_segment = free_segments_.rbegin();
        if (last_free_segment == free_segments_.rend()) return max_num_elements_;
        return last_free_segment->start_index;
    }
};

// TODO use a better and faster allocator for segments.
using SegmentAllocator = SimpleSegmentAllocator;

MI_NAMESPACE_END


#endif //SEGMENT_ALLOCATOR_H
