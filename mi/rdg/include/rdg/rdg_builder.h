/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_BUILDER_H
#define RDG_BUILDER_H

#include <set>

#include "core/types.h"
#include "rdg/rdg.h"
#include "rdg/rdg_param.h"

MI_NAMESPACE_BEGIN
class RDGPass;

enum class RDGPassFlagBits {
    // Do not cull this pass when compiling the graph
    kNeverCull = 1 << 0,
    // This pass is a graphics pass (dispatch some draw commands)
    kGraphics = 1 << 1,
    kAll = 0xffffffffu
};

MAKE_FLAGS(RDGPass);

class RenderGraphBuilder : public NonCopyable, public NonMovable {
public:
    void AddPass (
        const char *name,
        // Accessed parameters (meta and data)
        RDGShaderParamStructAndSizeInfo * shader_param_struct_info,
        void * parameter_struct,
        // Flags
        RDGPassFlags flags,
        std::function<void()> && pass) ;

    RenderGraphRef Compile ();

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
