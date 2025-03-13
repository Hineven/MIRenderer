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
#include "rhi/rhi_resource.h"
MI_NAMESPACE_BEGIN

// A resource that is imported into / exist only within a render graph
// Only the render thread can access its references, so no need for thread-safe reference counting.
class RDGResource : public NonCopyable, public RefCounted<false> {
public:
    friend class RDGResourcePool;
    FORCEINLINE RDGResource (RDGResourcePool * pool) : pool_(pool) {}
    virtual ~RDGResource () = default;
    // Get the hash value for mapping RDG resources to RHI resources. (classify resources)
    virtual uint32_t GetResourceClassHash () const = 0;
    // Release the underlying RHI resource to the pool
    virtual void ReleaseRHI () = 0;
    // Request the underlying RHI resource from the pool
    virtual void RequestRHI () = 0;
protected:
    RDGResourcePool * pool_ {};
};

typedef TRef<RDGResource> RDGResourceRef;

class RenderGraphTexture ;
class RenderGraphBuffer  ;

class RenderResourcePool : public NonCopyable, public NonMovable {
public:
    // TODO
};

class RDGBuffer;
class RDGTexture;

MI_NAMESPACE_END
#endif //MI_RDG_H
