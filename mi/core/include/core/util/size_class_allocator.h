/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef SIZE_CLASS_ALLOCATOR_H
#define SIZE_CLASS_ALLOCATOR_H

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include <core/common.h>
#include <core/util/segment_allocator.h>

MI_NAMESPACE_BEGIN

// =============================================================================
// SizeClassAllocator
// -----------------------------------------------------------------------------
// A size-class (slab) allocator over a flat index range, specialized for the
// "many small objects of similar size, high-frequency alloc/free" pattern
// (e.g. GigaVoxel chunk geometry: each chunk's baked mesh has a bounded vertex/
// index count, and chunks are streamed in/out constantly).
//
// Allocations are routed into one of N size classes. Each class owns a LIFO
// free-list of slots of a fixed slot_size. Allocate/Free hitting a class are
// O(1) (pop/push the free-list). Requests larger than the largest class fall
// back to a SimpleSegmentAllocator (first-fit, low-frequency big-block path).
//
// This is a PURE INDEX allocator — it hands out offsets into a logical range,
// it does not own any data. The caller mirrors the offsets onto its own CPU
// vector and GPU buffer (so CPU and GPU stay aligned by construction).
//
// Fragmentation control: Free'd slots return to their class free-list and are
// reused first-fit-by-LIFO. When Allocate cannot satisfy a request (no free
// slot and no room to carve a new slot at the tail) the caller may Compact()
// to defragment, or ExpandTo() to grow the range.
// =============================================================================
class SizeClassAllocator {
public:
    // Sentinel returned by Allocate on failure.
    static constexpr size_t kInvalidOffset = SIZE_MAX;

    // A record of one allocation that moved during Compact().
    struct Relocation {
        size_t old_offset;
        size_t new_offset;
        size_t size; // logical (requested) size, not slot size
    };

    // `capacity_elements` is the logical range size [0, capacity).
    // `class_sizes` must be non-empty and is stored sorted ascending; each is a
    // slot size in elements. A request of N elements is served by the smallest
    // class with slot_size >= N; requests exceeding the largest class use the
    // big-block fallback.
    // `alignment` aligns both slot sizes and big-block allocations.
    SizeClassAllocator(size_t capacity_elements,
                       std::initializer_list<uint32_t> class_sizes,
                       uint32_t alignment = 1)
        : capacity_(capacity_elements), alignment_(alignment) {
        mi_assert(class_sizes.size() > 0, "Need at least one size class.");
        class_sizes_.assign(class_sizes.begin(), class_sizes.end());
        std::sort(class_sizes_.begin(), class_sizes_.end());
        // Deduplicate.
        class_sizes_.erase(std::unique(class_sizes_.begin(), class_sizes_.end()),
                           class_sizes_.end());
        // Align slot sizes up.
        for (auto & s : class_sizes_) {
            s = AlignUp(s, alignment_);
        }
        free_lists_.assign(class_sizes_.size(), {});
        big_block_allocator_ = std::make_unique<SimpleSegmentAllocator>(capacity_, alignment_);
        // Tail cursor for carving fresh slots (only grows; freed slots recycle
        // via the free-lists before we carve new ones).
        carve_cursor_ = 0;
    }

    // Allocate a range of at least `num_elements`. Returns kInvalidOffset on
    // failure (caller should Compact() / ExpandTo() and retry). O(1) on the
    // class path, O(free-segments) on the big-block path.
    size_t Allocate(size_t num_elements) {
        if (num_elements == 0) num_elements = 1; // never hand out zero-size
        uint32_t cls = PickClass(num_elements);
        if (cls != kBigClass) {
            auto & fl = free_lists_[cls];
            if (!fl.empty()) {
                size_t off = fl.back();
                fl.pop_back();
                size_t slot = class_sizes_[cls];
                live_.emplace(off, AllocRecord{cls, slot, false});
                return off;
            }
            // No free slot: carve a fresh one from the tail.
            size_t slot = class_sizes_[cls];
            if (carve_cursor_ + slot > capacity_) return kInvalidOffset;
            size_t off = carve_cursor_;
            carve_cursor_ += slot;
            live_.emplace(off, AllocRecord{cls, slot, false});
            return off;
        }
        // Big-block fallback.
        size_t off = big_block_allocator_->Allocate(num_elements);
        if (off == SIZE_MAX) return kInvalidOffset;
        live_.emplace(off, AllocRecord{kBigClass, num_elements, true});
        return off;
    }

    // Free a previously allocated range. `num_elements` is the originally
    // requested size (used only for the big-block path; class path looks the
    // record up by offset). O(1) on the class path.
    void Free(size_t offset, size_t num_elements) {
        auto it = live_.find(offset);
        mi_assert(it != live_.end(), "Freeing an offset that is not live.");
        if (it == live_.end()) return;
        AllocRecord rec = it->second;
        live_.erase(it);
        if (rec.is_big) {
            big_block_allocator_->Free(offset, rec.slot_size);
        } else {
            free_lists_[rec.class_idx].push_back(offset);
        }
        (void)num_elements; // class path ignores; big path used rec.slot_size
    }

    // Defragment: compact all live allocations towards the front and reclaim
    // the tail. Returns the list of relocations (old->new) so the caller can
    // fix up its mirrors (CPU vector, GPU buffer, per-chunk handles). Resets
    // the big-block allocator. O(L log L) where L = live count.
    std::vector<Relocation> Compact() {
        std::vector<Relocation> relocations;
        // Snapshot live records sorted by offset.
        std::vector<std::pair<size_t, AllocRecord>> sorted;
        sorted.reserve(live_.size());
        for (auto & [off, rec] : live_) sorted.emplace_back(off, rec);
        std::sort(sorted.begin(), sorted.end(),
                  [](auto & a, auto & b) { return a.first < b.first; });

        // Rebuild allocator state.
        live_.clear();
        for (auto & fl : free_lists_) fl.clear();
        big_block_allocator_ = std::make_unique<SimpleSegmentAllocator>(capacity_, alignment_);
        carve_cursor_ = 0;

        for (auto & [old_off, rec] : sorted) {
            size_t new_off;
            if (rec.is_big) {
                new_off = big_block_allocator_->Allocate(rec.slot_size);
                mi_assert(new_off != SIZE_MAX, "Compact big-block alloc failed (should fit).");
            } else {
                // Carve contiguously; no free-list reuse during compaction.
                mi_assert(carve_cursor_ + rec.slot_size <= capacity_,
                          "Compact class carve exceeded capacity.");
                new_off = carve_cursor_;
                carve_cursor_ += rec.slot_size;
            }
            live_.emplace(new_off, rec);
            if (new_off != old_off) {
                relocations.push_back({old_off, new_off, rec.slot_size});
            }
        }
        return relocations;
    }

    // Grow the logical range. Existing allocations stay in place; new space is
    // appended at the tail (carve_cursor / big-block range extends).
    void ExpandTo(size_t new_capacity) {
        if (new_capacity <= capacity_) return;
        big_block_allocator_->ExpandTo(new_capacity);
        capacity_ = new_capacity;
    }

    FORCEINLINE size_t GetCapacity() const { return capacity_; }

    // Highest offset+1 that is (or was) occupied. Useful for sizing GPU buffers
    // and bounding BLAS builds.
    size_t GetHighWatermark() const {
        size_t hi = carve_cursor_;
        // Big blocks live at the tail region of the big-block allocator; its
        // max allocation end may exceed carve_cursor_.
        size_t big_hi = big_block_allocator_->GetMaxAllocationEndOffset();
        return std::max(hi, big_hi);
    }

    // Number of live allocations.
    FORCEINLINE size_t GetLiveCount() const { return live_.size(); }

    // True if no allocations are outstanding.
    FORCEINLINE bool IsEmpty() const { return live_.empty(); }

private:
    static constexpr uint32_t kBigClass = UINT32_MAX;

    FORCEINLINE static uint32_t AlignUp(uint32_t v, uint32_t a) {
        if (a <= 1) return v;
        return static_cast<uint32_t>((v + a - 1) / a * a);
    }

    // Smallest class whose slot_size >= num_elements, or kBigClass if none.
    uint32_t PickClass(size_t num_elements) const {
        for (uint32_t i = 0; i < class_sizes_.size(); ++i) {
            if (class_sizes_[i] >= num_elements) return i;
        }
        return kBigClass;
    }

    size_t capacity_ {};
    uint32_t alignment_ {};
    std::vector<uint32_t> class_sizes_;          // ascending, aligned
    std::vector<std::vector<size_t>> free_lists_;// per-class LIFO free offsets
    std::unique_ptr<SimpleSegmentAllocator> big_block_allocator_;
    size_t carve_cursor_ {};                      // next fresh-slot offset (class path)

    struct AllocRecord {
        uint32_t class_idx; // kBigClass for big-block
        size_t   slot_size; // actual reserved size (slot size or big request)
        bool     is_big;
    };
    std::unordered_map<size_t, AllocRecord> live_;
};

MI_NAMESPACE_END

#endif // SIZE_CLASS_ALLOCATOR_H
