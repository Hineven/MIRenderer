/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_RESOURCE_H
#define MIRENDERER_RHI_RESOURCE_H

#include <atomic>
#include "core/refcounted.h"
#include "core/base.h"
#include "core/thr.h"
#include "rhi/rhi_common.h"
#include "rhi/rhi_types.h"

MI_NAMESPACE_BEGIN

class RHIBindlessSlotKeeperBase;

class RHIResource : public NonMovable, public NonCopyable {
public:
    virtual ~RHIResource() ;

    FORCEINLINE uint32_t IncRef() {
        VerifyOwnerThread();
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() {
        VerifyOwnerThread();
        ref_count_--;
        if (ref_count_ == 0) {
            QueueForDeletion();
        }
        return ref_count_;
    }

    FORCEINLINE uint32_t GetRefCount() const {
        return ref_count_;
    }

    FORCEINLINE RHIResourceFlags GetFlags() const {
        return flags_;
    }

    virtual void * GetAPIHandle () const = 0;

    // Usually used for debugging
    virtual void SetName (const std::string & name) ;

    FORCEINLINE const char * GetName () const {
#ifndef NDEBUG
        return name_.c_str();
#else
        return "";
#endif
    }

    // Reference counts of a RHI resource can only be modified via its owning thread.
    // The function is a shortcut verifying that the current thread is the owner of the resource.
    FORCEINLINE void VerifyOwnerThread () const {
        assert(owner_thread_ == GetCurrentThreadType());
    }

    // Transfer the ownership of the resource to the current thread.
    FORCEINLINE void UpdateOwner () {
#ifndef NDEBUG
        owner_thread_ = GetCurrentThreadType();
#endif
    }

protected:

    // Can only be allocated by RHI and memory is allocated via infrastructure.
    RHIResource() ;
    // Queue up in a global list for deletion.
    // Queued resources will be deleted after (but not immediately after)
    // their frame ends execution on the device.
    void QueueForDeletion () ;

    // Only a single thread (the render thread, or the RHI thread) is allowed to operate on RHI resource references
    // for its entire lifetime.
    // so no need for atomic operations
    uint32_t ref_count_ {0};

#ifndef NDEBUG
    // For validation purposes only
    ThreadType owner_thread_ {};
#endif

    // Flags
    RHIResourceFlags flags_ {};

private:
#ifndef NDEBUG
    std::string name_ {};
#endif
};

// Called within RHI thread. Resources that are at least 1 frame older than
// the current frame in the pending queue will be recycled. By this we ensure
// that they are really no longer used by host or device.
void RecycleRHIResourcesPendingForDeletion_RHIThread();

// Only the render thread is allowed to operate on RHI resource references
using RHIResourceRef = TRef<RHIResource>;

class RHISampler : public RHIResource {
public:
    FORCEINLINE RHISampler(RHISamplerFilterType filter, RHISamplerAddressModeType addressing):
    filter_(filter), addressing_(addressing) {}
    virtual ~RHISampler() = default;
protected:
    RHISamplerFilterType filter_;
    RHISamplerAddressModeType addressing_;
};

class RHISyncPoint : public RHIResource {
protected:
    inline RHISyncPoint () = default;
    inline virtual ~RHISyncPoint () = default;
public:
    // Wait for the corresponding device task to finish
    virtual void Wait () = 0;
    // Reset the sync point so it can be reused
    virtual void Reset () = 0;
protected:
    // Notified from RHI thread upon submission completion, used for reordering waits and queue.submit().
    // (But the GPU may still be running on submitted instructions and the sync point is not yet reached)
    virtual void NotifySubmission () = 0;
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_RESOURCE_H
