/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_H
#define MI_RDG_H

#include <functional>
#include "core/common.h"
#include "core/base.h"
#include "rdg/rdg_fwd.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_type_helpers.h"
MI_NAMESPACE_BEGIN
    // A resource that is imported into / exist only within a render graph
// Only the render thread can access its references, so no need for thread-safe reference counting.
class RDGResource : public NonCopyable, public NonMovable, public RefCounted<false> {
public:
    friend class RDGResourcePool;
    friend class RenderGraph;
    RDGResource () ;
    virtual ~RDGResource () ;
    // Get the hash value for mapping RDG resources to RHI resources. (classify resources)
    virtual uint32_t GetResourceClassHash () const = 0;
    // Release the underlying RHI resource to the pool
    virtual void ReleaseRHI () = 0;
    // Request the underlying RHI resource from the pool
    virtual void RequestRHI (RDGResourcePool * pool) = 0;

    FORCEINLINE bool IsImported () const {return flags_ & RDGResourceFlagBits::kImported;}

    // This should never be called internally by the RDG or in pass lambdas!
    FORCEINLINE void SetExport () {
        assert(!RDG_IsInRDGExecution() && execution_ref_counter < 1);
        flags_ = flags_ | RDGResourceFlagBits::kExport;
        // Set the execution_ref_counter to 1, so that the resource will not be evicted from the pool.
        execution_ref_counter = 1;
    }
    FORCEINLINE RDGResourceFlags GetFlags () const {return flags_;}

    FORCEINLINE RHIPipelineStageFlags GetReadStages () const { return read_stages_; }
    FORCEINLINE RHIPipelineStageFlags GetWriteStages () const {return write_stages_;}
    FORCEINLINE RHIGPUAccessFlags GetReadAccess () const { return read_access_; }
    FORCEINLINE RHIGPUAccessFlags GetWriteAccess () const {return write_access_;}
    FORCEINLINE void Use (RHIPipelineStageFlags stages, RHIGPUAccessFlags usage) {
        if (usage & RHIGPUAccessFlagBits::kWrite) {
            // Reset the "un-barriered" read access, because a xx-w barrier is assumed to be placed before Use(write).
            // Because a write access will always be barrier by following acceses, successive barriers will form
            // a "barrier chain" to ensure correct memory order.
            read_stages_ = RHIPipelineStageFlagBits::kNone;
            read_access_ = RHIGPUAccessFlagBits::kNone;
            write_stages_ = stages;
            write_access_ = write_access_ | GetWriteAccessFlags(usage);
        }
        if (usage & RHIGPUAccessFlagBits::kRead) {
            read_stages_ = read_stages_ | stages;
            read_access_ = read_access_ | GetReadAccessFlags(usage);
        }
    }

protected:

    // Number of passes that this resource is used in. Should only be used internally by RDG Execute().
    uint32_t execution_ref_counter {0};

    RDGResourceFlags flags_ {};
    // The pool that allocated RHI resources for this render graph resource
    TRef<RDGResourcePool> pool_;

    // Track the "un-barriered" access of the buffer, used for barrier placement.
    RHIPipelineStageFlags read_stages_ {};
    RHIPipelineStageFlags write_stages_ {};
    RHIGPUAccessFlags  read_access_ {};
    RHIGPUAccessFlags  write_access_ {};
};

FORCEINLINE RDGPassType GetRDGPassType (RHIPipelineType type) {
    switch (type) {
        case RHIPipelineType::kGraphics: return RDGPassType::kGraphics;
        case RHIPipelineType::kCompute: return RDGPassType::kCompute;
        default:
            assert(false && "Not implemented");
            return RDGPassType::kMax;
    }
}

const char * ToCString (RDGPassType type) ;

template<CPointerType T>
FORCEINLINE bool RDGParameter_IsUnsetPointer (T ptr) {
    return reinterpret_cast<uint64_t>(ptr) == RDGParameter_UnsetPointer;
}

MI_NAMESPACE_END
#endif //MI_RDG_H
