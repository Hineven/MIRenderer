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

// The builder class for building a render graph.
// It holds all the passes and resources created during the building phase.
// After building, call Compile() to get a RenderGraph object that can be executed.
// The builder itself can be discarded after Compile() is called.
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
    // Useful for transferring data to pass lambdas
    FORCEINLINE void * Allocate (size_t size) {
        return allocator_->Allocate(size);
    }

    // Allocate temporary memory that lives up to the end of the graph execution.
    // Useful for transferring data to pass lambdas
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
        RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess,
        uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        return CreateTexture(RHITextureDesc{
            RHITextureType::k2D,
            {width, height, 1},
            mip_levels, array_layers, format, usage
        });
    }

    FORCEINLINE TRef<RDGTexture> CreateTexture2D (
    glm::uvec2 dimensions, PixelFormatType format,
    RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess,
    uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        return CreateTexture(RHITextureDesc{
            RHITextureType::k2D,
            {dimensions.x, dimensions.y, 1},
            mip_levels, array_layers, format, usage
        });
    }

    // Import a rhi texture. NOTE: The builder kept a reference to the resource once imported.
    // Importing the same RHI texture multiple times will return the same RDGTexture. So feel free to use it anywhere.
    // If the layout is kUndefined, we don't care about the contents of the imported texture.
    // All the data will be lost within the first pass that uses it.
    RDGTexture * Import (RHITexture * resource, RHITextureLayoutType layout = RHITextureLayoutType::kUndefined,
    RHIGPUAccessFlags prev_access = RHIGPUAccessFlagBits::kNone, RHIPipelineStageFlags prev_stages = RHIPipelineStageFlagBits::kNone) ;

    // Create a RDG buffer with the given description. Shortcut for RDGBuffer::Create
    TRef<RDGBuffer> CreateBuffer (RHIBufferUsageFlags usage, size_t size, bool dedicated = false, bool no_warning = false) ;
    // Legacy support
    template <CMemTrivial T>
    FORCEINLINE TRef<RDGBuffer> CreateBuffer (RHIBufferUsageFlags usage,
        size_t count = 1, bool dedicated = false, bool no_warning = false) {
        return CreateBuffer(usage, sizeof(T) * count, dedicated, no_warning);
    }

    // Create a RDG buffer with the given description. Shortcut for RDGBuffer::Create
    template <CMemTrivial T>
    FORCEINLINE TRef<RDGBuffer> CreateBuffer (size_t count = 1, RHIBufferUsageFlags usage = RHIBufferUsageFlagBits::kStorage,
     bool dedicated = false, bool no_warning = false) {
        return CreateBuffer(usage, sizeof(T) * count, dedicated, no_warning);
    }

    // Import a rhi buffer. NOTE: The builder kept a reference to the resource once imported.
    RDGBuffer * Import (RHIBuffer * resource, RHIGPUAccessFlags prev_access = RHIGPUAccessFlagBits::kNone, RHIPipelineStageFlags prev_stages = RHIPipelineStageFlagBits::kNone) ;

    FORCEINLINE void PushPassClassPath (const std::string & class_name) {
        current_class_path_.push_back(class_name);
    }

    FORCEINLINE void PopPassClassPath () {
        if (!current_class_path_.empty()) {
            current_class_path_.pop_back();
        }
    }

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

    // Current class path of the passes being added. Used for debug naming.
    std::vector<std::string> current_class_path_;
};

// Easily manage push/pop of pass class path in the builder.
// Usage:
// {
//     RDGSectionGuard section(builder, "MySection");
//     // All passes added here will have "MySection" in their class path.
//     ...
// }
class RDGSectionGuard {
public:
    FORCEINLINE RDGSectionGuard(RenderGraphBuilder & builder, const std::string & name) :
    name_(name), builder_(builder) {
        builder_.PushPassClassPath(name_);
    }
    FORCEINLINE ~RDGSectionGuard() {
        builder_.PopPassClassPath();
    }

    // Remove copy and move ctors and assignments
    RDGSectionGuard(const RDGSectionGuard &) = delete;
    RDGSectionGuard & operator=(const RDGSectionGuard &) = delete;
    RDGSectionGuard(RDGSectionGuard &&) = delete;
    RDGSectionGuard & operator=(RDGSectionGuard &&) = delete;
protected:
    std::string name_;
    RenderGraphBuilder & builder_;
};

MI_NAMESPACE_END

#endif //RDG_BUILDER_H
