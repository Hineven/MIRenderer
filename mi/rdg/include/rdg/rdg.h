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
class RDGResource : public NonCopyable, public RefCounted<> {
public:
    RDGResource (RHIResource * resource) : resource_(resource) {}
    RHIResource * GetResource () const { return resource_; }
protected:
    // Kept by raw pointer. The resource won't be destroyed until the commands from next frame
    // start submitting even if it has 0 refcount. So it's safe.
    RHIResource * resource_;
};

typedef TRef<RDGResource> RDGResourceRef;

class RenderGraphTexture ;
class RenderGraphBuffer  ;

class RenderResourcePool : public NonCopyable, public NonMovable {
public:

};

class RenderGraph : public RefCounted<> {
public:
    void Execute (RenderResourcePool & pool) ;
};

typedef TRef<RenderGraph> RenderGraphRef;

class RenderGraphBuilder : public NonCopyable, public NonMovable {
public:
    void AddPass (const char *name, std::function<void()> pass) ;
    RenderGraphRef Compile ();

    void ImportResource (const char *name, TRef<RHIResource> resource) ;

};

MI_NAMESPACE_END
#endif //MI_RDG_H
