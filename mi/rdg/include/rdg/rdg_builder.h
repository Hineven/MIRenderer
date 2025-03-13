/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_BUILDER_H
#define RDG_BUILDER_H

#include "rdg/rdg.h"

MI_NAMESPACE_BEGIN

class RenderGraphBuilder : public NonCopyable, public NonMovable {
public:
    void AddPass (const char *name, std::function<void(RenderGraph &)> pass) ;
    RenderGraphRef Compile ();

    RDGTexture ImportResource (const char *name, TRef<RHITexture> resource) ;
    RDGBuffer  ImportResource (const char *name, TRef<RHIBuffer> resource) ;
    RDGResource ImportResource (const char * name, TRef<RHIResource> resource) ;
    RDGTexture ExportResource (const char *name, RDGTexture texture) ;

};

MI_NAMESPACE_END

#endif //RDG_BUILDER_H
