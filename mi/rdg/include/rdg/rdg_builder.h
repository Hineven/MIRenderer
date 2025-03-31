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

    RDGPass * AddPass (
        const char *name,
        RDGPassType pass_type,
        RDGPassFlags pass_flags,
        // Accessed parameters (meta and data)
        const RDGShaderParamStructAndSizeInfo * shader_param_struct_info,
        void * parameter_struct,
        RDGPassLambda && pass) ;

    FORCEINLINE RDGPass * AddPass (
        const char *name,
        RDGPassFlags pass_flags,
        RDGPassLambda && pass
    ) {
        return AddPass(
            name,
            RDGPassType::kGeneric, pass_flags,
            nullptr, nullptr,
            std::move(pass)
        );
    }

    FORCEINLINE RDGPass * AddPass (
        RDGPassFlags pass_flags,
        RDGPassLambda && pass
) {
        return AddPass(
            "<anonymous generic pass>",
            RDGPassType::kGeneric, pass_flags,
            nullptr, nullptr,
            std::move(pass)
        );
    }

    template<typename T>
    FORCEINLINE RDGPass * AddPass (
        RDGPassFlags pass_flags,
        typename T::ShaderParameters * parameter_struct,
        RDGPassLambda && pass
    ) {
        return AddPass(
            T::GetShaderTypeName(),
            T::GetRDGPassType(), pass_flags,
            T::GetShaderParamStructInfo(), parameter_struct,
            std::move(pass)
        );
    }
    
    TRef<RenderGraph> Compile ();

    FORCEINLINE void * Allocate (size_t size) {
        return allocator_->Allocate(size);
    }
    template<CMemTrivial T>
    FORCEINLINE T * Allocate (bool zero = true) {
        auto ptr = static_cast<T*>(Allocate(sizeof(T)));
        if (zero) {
            memset(ptr, 0, sizeof(T));
        }
        new (ptr) T();
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
