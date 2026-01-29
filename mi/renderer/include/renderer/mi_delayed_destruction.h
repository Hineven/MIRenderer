/*
 * Created: 2026/1/29
 * Author:  GitHub Copilot (refactor helper)
 *
 * A tiny utility to centralize delayed destruction for renderer-side (non-RHIResource)
 * objects that must not be destroyed until the GPU is guaranteed to have finished
 * consuming them.
 */

#ifndef MI_DELAYED_DESTRUCTION_H
#define MI_DELAYED_DESTRUCTION_H

#include <vector>

#include <core/refcounted.h>

// For NonMovable/NonCopyable, mi_assert, FORCEINLINE
#include <core/base.h>

MI_NAMESPACE_BEGIN

// A base class that is TRef-compatible via intrusive refcounting (IncRef/DecRef),
// similar to RHIResource.
//
// Design:
// - Does NOT inherit RefCounted.
// - When refcount reaches 0, it calls QueueForDestruction() instead of delete this.
// - The owner (e.g. DeviceBindlessResourceAllocator) should keep a unique reference
//   for a few frames, then actually delete the object.
//
// Threading:
// - Like RHIResource, refcount operations are intended to be performed from a single
//   owning thread (typically render thread). No atomics here.
class DelayedDestructionResource : public NonMovable, public NonCopyable {
public:
    virtual ~DelayedDestructionResource() = default;

    FORCEINLINE uint32_t IncRef() const {
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() const {
        ref_count_--;
        if (ref_count_ == 0) {
            QueueForDestruction();
        }
        return ref_count_;
    }

    [[nodiscard]] FORCEINLINE uint32_t GetRefCount() const {
        return ref_count_;
    }

protected:
    DelayedDestructionResource() = default;

    // Called when external refs drop to 0.
    // Override this in derived types to route destruction through an owner-managed
    // delayed-destruction queue.
    virtual void QueueForDestruction() const = 0;

private:
    mutable uint32_t ref_count_ {0};
};

// Owner interface for delayed destruction.
// If you don't want to expose your concrete owner type, implement this and pass it to keepers.
class IDeferredFreeOwner {
public:
    virtual ~IDeferredFreeOwner() = default;
    // Take ownership of the object and delete it later.
    virtual void EnqueueForDelayedDestruction(DelayedDestructionResource *obj) = 0;
};

// Generic keeper for a "handle" (slot id, index, etc.) whose release must be delayed.
//
// Pattern:
// - The keeper is ref-counted (via DelayedDestructionResource).
// - When its refcount reaches 0, it enqueues itself into an owner-managed delayed destruction queue.
// - When the queue finally deletes it, its destructor runs and performs the actual release.
//
// This is reusable for DeviceBindlessResourceAllocator slot indices and for Scene renderable indices.
template<typename OwnerT>
class TDelayedReleaseKeeper : public DelayedDestructionResource {
 public:
    using ReleaseFn = void (*)(OwnerT *owner, uint32_t value);

    TDelayedReleaseKeeper(OwnerT *owner, uint32_t value, ReleaseFn release)
        : owner_(owner), value_(value), release_(release) {
        mi_check(owner_, "Owner must not be null.");
    }

    [[nodiscard]] FORCEINLINE uint32_t Get() const { return value_; }
    [[nodiscard]] FORCEINLINE OwnerT *GetOwner() const { return owner_; }

    void QueueForDestruction() const override {
        // Owner controls actual deletion timing.
        owner_->EnqueueForDelayedDestruction(this);
    }

 protected:
    ~TDelayedReleaseKeeper() override {
         if (release_ && value_ != UINT32_MAX) {
             release_(owner_, value_);
         }
     }

 private:
      OwnerT *owner_ {};
      uint32_t value_ {UINT32_MAX};
      ReleaseFn release_ {};
};

// A simple, render-thread-only delayed destruction queue.
// It stores raw pointers and deletes them after N ticks.
//
// This matches the `DelayedDestructionResource`/`RHIResource` pattern:
// - external refs drop to 0
// - object enqueues itself for destruction
// - an owner-managed queue deletes it a few frames later
//
// IMPORTANT: Enqueue() takes OWNERSHIP of the pointer. Do not delete it elsewhere.
template<typename T>
class TDelayedDestructionQueue {
public:
    static constexpr uint32_t kDefaultDelayFrames = 2;

    explicit TDelayedDestructionQueue(uint32_t delay_frames = kDefaultDelayFrames)
        : delay_frames_(delay_frames) {
        mi_assert(delay_frames_ >= 1 && delay_frames_ <= 8, "DelayFrames should be within a small range.");
        ring_.resize(delay_frames_ + 1); // +1 so that delay=2 means: push in bucket curr, release when bucket wraps.
    }

    void Enqueue(T * obj) {
        if (!obj) return;
        ring_[write_index_].push_back(obj);
    }

    // Call once per frame, at a point where frame N-1 has completed on GPU.
    void Tick() {
        // Advance ring and drop objects that reached their retirement bucket.
        write_index_ = (write_index_ + 1) % ring_.size();
        auto &bucket = ring_[write_index_];
        for (auto *e : bucket) {
            delete e;
        }
        bucket.clear();
    }

    void ClearAllNow() {
        for (auto &bucket : ring_) {
            for (auto *e : bucket) {
                delete e;
            }
            bucket.clear();
        }
    }

private:
    uint32_t delay_frames_;
    std::vector<std::vector<T*>> ring_;
    size_t write_index_ = 0;
};

// Default queue type for DelayedDestructionResource-like objects.
using DelayedDestructionQueue = TDelayedDestructionQueue<DelayedDestructionResource>;

MI_NAMESPACE_END

#endif // MI_DELAYED_DESTRUCTION_H
