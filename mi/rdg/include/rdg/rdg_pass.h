/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PASS_H
#define RDG_PASS_H
#include "rdg_resource.h"
#include "rdg/rdg_base.h"
MI_NAMESPACE_BEGIN
struct RDGShaderParamStructAndSizeInfo;

class RDGPass : public NonMovable, public NonCopyable {
protected:
    // Can only be allocated by RDG
    FORCEINLINE RDGPass(
        int index,
        RDGPassType pass_type,
        RDGPassFlags flags,
        std::function<void(RHICommandQueueGraphics&)> && pass,
        const RDGShaderParamStructAndSizeInfo * shader_param_struct_info,
        const void * shader_param_data,
        RDGBuffer * indirect_buffer = nullptr
    ) : index_(index),
        type_(pass_type),
        flags_(flags),
        pass_(std::move(pass)),
        shader_param_struct_info_(shader_param_struct_info),
        shader_param_data_(shader_param_data),
        indirect_buffer_(indirect_buffer) {
        GatherResourceAccesses();
    }
public:
    friend class RenderGraphBuilder;
    friend class RenderGraph;
    FORCEINLINE RDGPassFlags GetFlags () const {return flags_;}
    FORCEINLINE RDGPassType GetType () const {return type_;}
    FORCEINLINE RHIPipelineStageFlags GetStageFlags () const {
        switch (type_) {
            case RDGPassType::kGraphics:
                return RHIPipelineStageFlagBits::kOrdinaryGraphics;
            case RDGPassType::kCompute:
                return RHIPipelineStageFlagBits::kCompute;
            default:
                assert(false);
                return {};
        }
    }

protected:
    // The index when the pass is joined to the graph
    int index_;
    // Flags for the pass
    RDGPassFlags flags_ {};
    RDGPassType type_ {};
    const RDGShaderParamStructAndSizeInfo * shader_param_struct_info_;
    const void * shader_param_data_;

    // Grouping resources by access types
    std::vector<RDGTexture*> out_textures_;
    std::vector<RDGBuffer*> out_buffers_;
    std::vector<RDGTexture*> in_textures_;
    std::vector<RDGBuffer*> in_buffers_;
    // Indirect buffer
    RDGBufferRef indirect_buffer_ {nullptr};

    struct RDGTextureUsage {
        enum {
            kShaderRead,
            // Storage image
            kShaderReadWrite,
            kOutputAttachment,
            kDepthStencilAttachment
        } usage;
        RDGTextureRef texture;
    };
    // Details of each resource access, and reference holding
    std::vector<RDGTextureUsage> used_textures_;
    struct RDGBufferUsage {
        enum {
            kUniformBuffer,
            kReadOnlyStorge,
            kReadWriteStorage,
            kVertexBuffer,
            kIndexBuffer,
            kIndirectBuffer
        } usage;
        RDGBufferRef buffer;
    };
    // Details of each resource access, and reference holding
    std::vector<RDGBufferUsage> used_buffers_;

    // Private uniform buffer
    RDGBufferRef uniform_buffer_;
    // Uniform buffers referenced
    std::vector<RDGBufferRef> referenced_uniform_buffers_;

    // This is filled up by the RDG builder upon spawning the pass
    std::vector<RDGPass*> successive_passes_;

    // Gather resources accessed by the shader, initialize in/out resources and detailed resource usage
    void GatherResourceAccesses () ;

    std::function<void(RHICommandQueueGraphics &)> pass_;
};

MI_NAMESPACE_END

#endif //RDG_PASS_H
