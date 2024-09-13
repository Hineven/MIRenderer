/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_RESOURCE_H
#define MIRENDERER_RHI_RESOURCE_H

#include <atomic>
#include "core/refcounted.h"
#include "rhi/rhi_common.h"
#include "rhi/rhi_types.h"

MI_NAMESPACE_BEGIN

class RHIResource {
public:
    RHIResource() = default;
    virtual ~RHIResource() = default;

    FORCEINLINE uint32_t IncRef() {
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() {
        ref_count_--;
        if (ref_count_ == 0) {
            QueueForDeletion();
        }
        return ref_count_;
    }

    FORCEINLINE uint32_t GetRefCount() const {
        return ref_count_;
    }
private:

    // Queue up in a global list for deletion
    void QueueForDeletion () ;

    // Only the render thread is allowed to operate on RHI resource references
    // so no need for atomic operations
    uint32_t ref_count_ {0};

    bool pending_for_deletion_ {false};
};

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

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_RESOURCE_H
