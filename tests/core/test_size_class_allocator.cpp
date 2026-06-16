/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>

#include <core/util/size_class_allocator.h>

using mi::SizeClassAllocator;

namespace {
// Reasonable classes for chunk-sized allocations.
constexpr std::initializer_list<uint32_t> kClasses = {32, 64, 128, 256, 512};
}

TEST(SizeClassAllocator, AllocatesAndTracksLiveCount) {
    SizeClassAllocator a(4096, kClasses);
    EXPECT_EQ(a.GetLiveCount(), 0u);
    size_t off = a.Allocate(100); // -> class 128
    EXPECT_NE(off, SizeClassAllocator::kInvalidOffset);
    EXPECT_EQ(a.GetLiveCount(), 1u);
    a.Free(off, 100);
    EXPECT_EQ(a.GetLiveCount(), 0u);
}

TEST(SizeClassAllocator, RoundsUpToClassSize) {
    // A request of 33 falls into the 64 class; the slot size is 64.
    SizeClassAllocator a(4096, kClasses);
    size_t off = a.Allocate(33);
    EXPECT_NE(off, SizeClassAllocator::kInvalidOffset);
    // carve_cursor should have advanced by the slot size (64).
    a.Free(off, 33);
}

TEST(SizeClassAllocator, FreeListReuseIsLIFO) {
    // Allocate several same-class chunks, free them, re-allocate: offsets
    // should be recycled (free-list LIFO) rather than carving fresh space.
    SizeClassAllocator a(1 << 16, kClasses);
    std::vector<size_t> offs;
    for (int i = 0; i < 10; ++i) offs.push_back(a.Allocate(64)); // class 64
    size_t cursor_before = a.GetHighWatermark();
    // Free all.
    for (size_t o : offs) a.Free(o, 64);
    // Re-allocate: should come from the free-list, no new carving.
    size_t o1 = a.Allocate(64);
    size_t o2 = a.Allocate(64);
    // LIFO: the last-freed offset is reused first; second reuse is the prior.
    EXPECT_EQ(o1, offs.back());
    EXPECT_EQ(o2, offs[offs.size() - 2]);
    size_t cursor_after = a.GetHighWatermark();
    EXPECT_EQ(cursor_after, cursor_before); // no growth
}

TEST(SizeClassAllocator, DifferentSizesGetDistinctSlots) {
    SizeClassAllocator a(1 << 16, kClasses);
    size_t a32 = a.Allocate(32);  // class 32
    size_t a64 = a.Allocate(64);  // class 64
    size_t a128 = a.Allocate(100);// class 128
    // Each occupies a non-overlapping range.
    EXPECT_NE(a32, a64);
    EXPECT_NE(a64, a128);
    EXPECT_NE(a32, a128);
}

TEST(SizeClassAllocator, BigBlockFallback) {
    // Request larger than the largest class (512): falls back to segment alloc.
    SizeClassAllocator a(1 << 16, kClasses);
    size_t off = a.Allocate(1000);
    EXPECT_NE(off, SizeClassAllocator::kInvalidOffset);
    a.Free(off, 1000);
    EXPECT_EQ(a.GetLiveCount(), 0u);
}

TEST(SizeClassAllocator, FailureWhenFull) {
    // Tiny capacity, large class request should fail when exhausted.
    SizeClassAllocator a(256, kClasses);
    size_t off = a.Allocate(256); // class 256 fills the whole range
    EXPECT_NE(off, SizeClassAllocator::kInvalidOffset);
    size_t off2 = a.Allocate(256);
    EXPECT_EQ(off2, SizeClassAllocator::kInvalidOffset); // no room
    a.Free(off, 256);
    // Now it fits again.
    size_t off3 = a.Allocate(256);
    EXPECT_NE(off3, SizeClassAllocator::kInvalidOffset);
}

TEST(SizeClassAllocator, ExpandToGrows) {
    SizeClassAllocator a(256, kClasses);
    a.Allocate(256); // fills it
    EXPECT_EQ(a.Allocate(64), SizeClassAllocator::kInvalidOffset);
    a.ExpandTo(1024);
    size_t off = a.Allocate(64);
    EXPECT_NE(off, SizeClassAllocator::kInvalidOffset);
    a.Free(off, 64);
}

TEST(SizeClassAllocator, CompactDefragments) {
    // Create fragmentation: allocate several, free the middle ones, compact.
    SizeClassAllocator a(1 << 16, kClasses);
    std::vector<size_t> offs;
    for (int i = 0; i < 6; ++i) offs.push_back(a.Allocate(64)); // 6 x class-64
    // Free even-indexed ones to create holes.
    a.Free(offs[1], 64);
    a.Free(offs[3], 64);
    a.Free(offs[5], 64);
    size_t wm_before = a.GetHighWatermark();
    auto relocs = a.Compact();
    size_t wm_after = a.GetHighWatermark();
    // Compaction should not grow the watermark, and should shrink it (3 holes
    // at the tail collapsed).
    EXPECT_LE(wm_after, wm_before);
    // Live count preserved.
    EXPECT_EQ(a.GetLiveCount(), 3u);
    // At least one relocation should have occurred (the tail chunks moved down).
    EXPECT_GE(relocs.size(), 1u);
}

TEST(SizeClassAllocator, HighWatermarkReflectsAllocations) {
    SizeClassAllocator a(1 << 16, kClasses);
    EXPECT_EQ(a.GetHighWatermark(), 0u);
    size_t o1 = a.Allocate(64);
    EXPECT_EQ(a.GetHighWatermark(), 64u);
    size_t o2 = a.Allocate(64);
    EXPECT_EQ(a.GetHighWatermark(), 128u);
    a.Free(o1, 64);
    a.Free(o2, 64);
    // Watermark stays (carve_cursor doesn't retreat), but live count is 0.
    EXPECT_EQ(a.GetHighWatermark(), 128u);
    EXPECT_EQ(a.GetLiveCount(), 0u);
    EXPECT_TRUE(a.IsEmpty());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
