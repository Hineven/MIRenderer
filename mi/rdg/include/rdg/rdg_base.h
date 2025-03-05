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
    FORCEINLINE void Use (RHIGPUAccessFlags access) { accumulated_access_ = accumulated_access_ | access; }
    FORCEINLINE RHIGPUAccessFlags GetAccessFlags () const { return accumulated_access_; }
protected:
    // Currently accumulated access flags on this resource
    RHIGPUAccessFlags accumulated_access_ {};
    // Kept by raw pointer. The resource won't be destroyed until the commands from next frame
    // start submitting even if it has 0 refcount. So it's safe.
    RHIResource * resource_;
};

class RDGPool : public NonCopyable, public NonMovable {
public:
    RDGPool () {};
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
