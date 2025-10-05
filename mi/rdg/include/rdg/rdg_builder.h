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
        RDGShader * shader,
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
            nullptr,
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
            nullptr,
            nullptr, nullptr,
            std::move(pass)
        );
    }

    // Add a pass for shader dispatches
    template<typename T>
    FORCEINLINE RDGPass * AddPass (
        RDGPassFlags pass_flags,
        T * shader,
        typename T::ShaderParameters * parameter_struct,
        RDGPassLambda && pass
    ) {
        assert(shader != nullptr && "Shader must not be null");
        return AddPass(
            T::GetShaderTypeName(),
            T::GetRDGPassType(), pass_flags,
            shader,
            T::GetShaderParamStructInfo(),
            parameter_struct,
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
    template<CMemTrivial T, typename BoolType = bool>
    FORCEINLINE T * Allocate (BoolType zero_before_construction = true) {
        static_assert(std::is_same_v<BoolType, bool>, "The parameter to Allocate must be a bool "
                                                      "(zero_before_construction), this may be confusing with other Allocate() functions.");
        auto ptr = static_cast<T*>(Allocate(sizeof(T)));
        if (zero_before_construction) {
            memset(ptr, 0, sizeof(T));
        }
        new (ptr) T();
        return ptr;
    }

    template<CAOUB T>
    FORCEINLINE std::remove_extent_t<T> * Allocate (uint32_t num_elements) {
        using elem_type = std::remove_extent_t<T>;
        auto ptr = static_cast<elem_type*>(Allocate(sizeof(elem_type) * num_elements));
        for (uint32_t i = 0; i < num_elements; i++) new (ptr + i) elem_type();
        return ptr;
    }

    // Create a RDG texture with the given description. Shortcut for RDGTexture::Create
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
    // Creat a 2D RDG texture array with the given description.
    FORCEINLINE TRef<RDGTexture> CreateTexture2DArray (
        uint32_t width, uint32_t height, uint32_t layers, PixelFormatType format,
        RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess) {
        return CreateTexture(RHITextureDesc{
            RHITextureType::k2D,
            {width, height, 1},
            1, layers, format, usage
        });
    }

    // Import a rhi texture. NOTE: The builder kept a reference to the resource once imported.
    // If the layout is kUndefined, we don't care about the contents of the imported texture.
    // All the data will be lost within the first pass that uses it.
    RDGTexture * Import (RHITexture * resource, RHITextureLayoutType layout = RHITextureLayoutType::kUndefined,
    RHIGPUAccessFlags prev_access = RHIGPUAccessFlagBits::kNone, RHIPipelineStageFlags prev_stages = RHIPipelineStageFlagBits::kNone) ;

    // Create a RDG buffer with the given description. Shortcut for RDGBuffer::Create
    TRef<RDGBuffer> CreateBuffer (RHIBufferUsageFlags usage, size_t size, bool dedicated = false, bool no_warning = false) ;
    template <CMemTrivial T>
    FORCEINLINE TRef<RDGBuffer> CreateBuffer (RHIBufferUsageFlags usage, size_t count = 1, bool dedicated = false, bool no_warning = false) {
        return CreateBuffer(usage, sizeof(T) * count, dedicated, no_warning);
    }

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
