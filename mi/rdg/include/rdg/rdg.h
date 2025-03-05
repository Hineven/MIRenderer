/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_H
#define RDG_H

#include "rdg_base.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN
class RenderGraph : public RefCounted<> {
public:
    void Execute (RenderResourcePool & pool) ;
};

typedef TRef<RenderGraph> RenderGraphRef;

class RenderGraphBuilder : public NonCopyable, public NonMovable {
public:
    void AddPass (const char *name, std::function<void()> pass) ;
    RenderGraphRef Compile ();

    RDGTexture ImportResource (const char *name, TRef<RHITexture> resource) ;
    RDGBuffer  ImportResource (const char *name, TRef<RHIBuffer> resource) ;
    RDGResource ImportResource (const char * name, TRef<RHIResource> resource) ;
    RDGTexture ExportResource (const char *name, RDGTexture texture) ;

};

MI_NAMESPACE_END

#endif //RDG_H
