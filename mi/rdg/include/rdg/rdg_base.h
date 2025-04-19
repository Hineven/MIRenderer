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
MI_NAMESPACE_BEGIN

// A resource that is imported into / exist only within a render graph
// Only the render thread can access its references, so no need for thread-safe reference counting.
class RDGResource : public NonCopyable, public NonMovable, public RefCounted<false> {
public:
    friend class RDGResourcePool;
    RDGResource () ;
    virtual ~RDGResource () ;
    // Get the hash value for mapping RDG resources to RHI resources. (classify resources)
    virtual uint32_t GetResourceClassHash () const = 0;
    // Release the underlying RHI resource to the pool
    virtual void ReleaseRHI () = 0;
    // Request the underlying RHI resource from the pool
    virtual void RequestRHI (RDGResourcePool * pool) = 0;

    FORCEINLINE bool IsImported () const {return flags_ & RDGResourceFlagBits::kImported;}

protected:
    RDGResourceFlags flags_ {};
    // The pool that allocated RHI resources for this render graph resource
    TRef<RDGResourcePool> pool_;
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
