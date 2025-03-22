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

enum class RDGPassFlagBits : unsigned {
    // Do not cull this pass when compiling the graph
    kNeverCull = 1 << 0,
    kAll = 0xffffffffu
};

MAKE_FLAGS(RDGPass);

// A resource that is imported into / exist only within a render graph
// Only the render thread can access its references, so no need for thread-safe reference counting.
class RDGResource : public NonCopyable, public RefCounted<false> {
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
protected:
    // The pool that allocated RHI resources for this render graph resource
    TRef<RDGResourcePool> pool_;
};

typedef TRef<RDGResource> RDGResourceRef;

class RenderGraphTexture ;
class RenderGraphBuffer  ;

class RDGBuffer;
class RDGTexture;

enum class RDGPassType {
    // Invoking draw commands
    kGraphics,
    // Compute shader
    kCompute,
    // Basic RHI commands
    kGeneric,
    // TODO add more (mesh, raytracing, etc)
    kMax
};

const char * ToCString (RDGPassType type) ;


class RDGPass;
class RHICommandQueueGraphics;
typedef std::function<void(RDGPass*, RHICommandQueueGraphics&)> RDGPassLambda;

MI_NAMESPACE_END
#endif //MI_RDG_H
