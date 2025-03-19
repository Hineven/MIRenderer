/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_BUILDER_H
#define RDG_BUILDER_H

#include <set>

#include "core/types.h"
#include "rdg/rdg_param.h"

MI_NAMESPACE_BEGIN
class RDGPass;
class RenderGraph;
typedef TRef<RDGResource> RDGResourceRef;

class RenderGraphBuilder : public NonCopyable, public NonMovable {
public:
    void AddPass (
        const char *name,
        RDGPassType pass_type,
        RDGPassFlags pass_flags,
        // Accessed parameters (meta and data)
        RDGShaderParamStructAndSizeInfo * shader_param_struct_info,
        void * parameter_struct,
        std::function<void(RHICommandQueueGraphics&)> && pass) ;

    TRef<RenderGraph> Compile ();

    RDGBuffer  ImportResource (const char *name, TRef<RHIBuffer> resource) ;
    RDGTexture ImportResource (const char *name, TRef<RHITexture> resource) ;

    RDGBuffer  ExportResource (const char *name, TRef<RHIBuffer> resource) ;
    RDGTexture ExportResource (const char *name, TRef<RDGTexture> resource) ;
protected:
    std::vector<std::unique_ptr<RDGPass>> passes_;
    std::set<RDGResource*> exporting_resources_;
    int current_pass_index_ {};

};

MI_NAMESPACE_END

#endif //RDG_BUILDER_H
