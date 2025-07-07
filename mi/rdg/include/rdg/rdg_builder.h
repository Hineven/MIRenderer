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

    // Add a pass
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

    // Add a pass (shortcut for generic passes)
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

    // Add a pass for shader dispatches
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
    
    TRef<RenderGraph> Compile (const std::string & name = "Unnamed Render Graph") ;

    // Allocate temporary memory that lives up to the end of the graph execution.
    // Useful for trasfering data to pass lambdas
    FORCEINLINE void * Allocate (size_t size) {
        return allocator_->Allocate(size);
    }

    // Allocate temporary memory that lives up to the end of the graph execution.
    // Useful for trasfering data to pass lambdas
    template<CMemTrivial T>
    FORCEINLINE T * Allocate (bool zero_before_construction = true) {
        auto ptr = static_cast<T*>(Allocate(sizeof(T)));
        if (zero_before_construction) {
            memset(ptr, 0, sizeof(T));
        }
        new (ptr) T();
        return ptr;
    }

    // Create a RDG texture with the given description.
    TRef<RDGTexture> CreateTexture (RHITextureDesc desc) ;
    // Create a 2D RDG texture with the given description.
    FORCEINLINE TRef<RDGTexture> CreateTexture2D (
        uint32_t width, uint32_t height, PixelFormatType format,
        RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess) {
        return CreateTexture(RHITextureDesc{
            RHITextureType::k2D,
            {width, height, 1},
            1, 1, format, usage
        });
    }

    // Import a rhi texture. NOTE: The builder kept a reference to the resource once imported.
    // If the layout is kUndefined, we don't care about the contents of the imported texture.
    // All the data will be lost within the first pass that uses it.
    RDGTexture * Import (RHITexture * resource, RHITextureLayoutType layout = RHITextureLayoutType::kUndefined,
    RHIGPUAccessFlags prev_access = RHIGPUAccessFlagBits::kNone, RHIPipelineStageFlags prev_stages = RHIPipelineStageFlagBits::kNone) ;

    // Create a RDG buffer with the given description.
    TRef<RDGBuffer> CreateBuffer (RHIBufferUsageFlags usage, size_t size, bool dedicated = false, bool no_warning = false) ;
    // Import a rhi buffer. NOTE: The builder kept a reference to the resource once imported.
    RDGBuffer * Import (RHIBuffer * resource, RHIGPUAccessFlags prev_access = RHIGPUAccessFlagBits::kNone, RHIPipelineStageFlags prev_stages = RHIPipelineStageFlagBits::kNone) ;



protected:

    std::unique_ptr<TOneTimeLinearAllocator<>> allocator_;

#ifndef NDEBUG
    std::map<const void *, uint32_t> param_struct_ptr_to_data_crc;
#endif

    // RHIResource ptr -> imported RDGBuffer
    std::unordered_map<void *, TRef<RDGBuffer>> external_buffer_map_;
    // RHIResource ptr -> imported RDGTexture
    std::unordered_map<void *, TRef<RDGTexture>> external_texture_map_;

    std::vector<std::unique_ptr<RDGPass>> passes_;
    int current_pass_index_ {};

};

MI_NAMESPACE_END

#endif //RDG_BUILDER_H
