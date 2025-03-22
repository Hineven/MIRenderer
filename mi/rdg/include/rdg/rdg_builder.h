/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_BUILDER_H
#define RDG_BUILDER_H

#include <set>

#include "core/types.h"
#include "core/util/alloc.h"
#include "rhi/rhi_desc.h"
#include "rdg/rdg_param.h"
#include "rdg/rdg_resource.h"
#include "rdg/rdg_pass.h"

MI_NAMESPACE_BEGIN
class RDGPass;
class RenderGraph;
typedef TRef<RDGResource> RDGResourceRef;

class RenderGraphBuilder : public NonCopyable, public NonMovable {
public:
    RenderGraphBuilder();
    ~RenderGraphBuilder();

    void AddPass (
        const char *name,
        RDGPassType pass_type,
        RDGPassFlags pass_flags,
        // Accessed parameters (meta and data)
        const RDGShaderParamStructAndSizeInfo * shader_param_struct_info,
        void * parameter_struct,
        RDGPassLambda && pass) ;


    TRef<RenderGraph> Compile ();

    TRef<RDGBuffer>  ImportResource (const char *name, TRef<RHIBuffer> resource) ;
    TRef<RDGTexture> ImportResource (const char *name, TRef<RHITexture> resource) ;

    TRef<RDGBuffer>  ExportResource (const char *name, TRef<RHIBuffer> resource) ;
    TRef<RDGTexture> ExportResource (const char *name, TRef<RDGTexture> resource) ;

    FORCEINLINE void * Allocate (size_t size) {
        return allocator_->Allocate(size);
    }
    template<CMemTrivial T>
    FORCEINLINE T * Allocate (bool zero = true) {
        auto ptr = static_cast<T*>(Allocate(sizeof(T)));
        if (zero) {
            memset(ptr, 0, sizeof(T));
        }
        return ptr;
    }
protected:

    std::unique_ptr<TOneTimeLinearAllocator<>> allocator_;

    std::vector<std::unique_ptr<RDGPass>> passes_;
    std::set<RDGResource*> exporting_resources_;
    int current_pass_index_ {};

};

MI_NAMESPACE_END

#endif //RDG_BUILDER_H
