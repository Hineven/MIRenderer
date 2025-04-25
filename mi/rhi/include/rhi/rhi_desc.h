/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_DESC_H
#define MI_RHI_DESC_H

#include <span>
#include <string>
#include <array>
#include <glm/detail/qualifier.hpp>

#include "rhi/rhi_fwd.h"
#include "rhi_types.h"
#include "core/pixel_format.h"
#include "core/constants.h"

MI_NAMESPACE_BEGIN

struct RHIParamStructInfo;

struct RHIBufferSpan {
    RHIBuffer * buffer;
    size_t offset;
    size_t size;
    FORCEINLINE bool IsValid () const {
        return buffer != nullptr;
    }

    FORCEINLINE bool operator==(const RHIBufferSpan & span) const = default;
    FORCEINLINE bool operator!=(const RHIBufferSpan & span) const = default;

    FORCEINLINE operator bool () const { // NOLINT
        return IsValid();
    }
};

struct RHIBindlessSupportInfo {
    uint32_t max_num_resource_slots;
    // uint32_t max_num_sampler_slots;
    // uint32_t max_num_immutable_sampler_slots;
    // Alignment for btb table
    uint32_t descriptor_buffer_offset_alignment;
};

struct RHIVertexInputBindingDesc {
    // Binding number
    uint32_t binding {0};
    // Stride for vertex data in bytes
    uint32_t stride {4 * 4};
    // Input rate for vertex data
    RHIVertexInputRateType input_rate {RHIVertexInputRateType::kVertex};
};

struct RHIVertexInputAttributeDesc {
    // Shader input location
    uint32_t location {0};
    // Bound vertex buffer binding number for data source
    uint32_t src_binding {0};
    // Data format
    RHIVertexAttributeFormatType format {RHIVertexAttributeFormatType::k4xFp32};
    // Byte offset relative to the start of the vertex data structure
    uint32_t offset {0};
};

struct RHIColorAttachmentBlendDesc {
    // Enable blending
    bool blend_enable {false};
    // Source blend factor
    RHIBlendFactorType src_color_blend_factor {RHIBlendFactorType::kOne};
    // Destination blend factor
    RHIBlendFactorType dst_color_blend_factor {RHIBlendFactorType::kZero};
    // Source blend factor for alpha
    RHIBlendFactorType src_alpha_blend_factor {RHIBlendFactorType::kOne};
    // Destination blend factor for alpha
    RHIBlendFactorType dst_alpha_blend_factor {RHIBlendFactorType::kZero};
    // Blend operation
    RHIBlendOpType color_blend_op {RHIBlendOpType::kBlendAdd};
    // Blend operation for alpha
    RHIBlendOpType alpha_blend_op {RHIBlendOpType::kBlendAdd};
};

struct RHIColorAttachmentDesc {
    RHIColorAttachmentBlendDesc blending;
    PixelFormatType format {PixelFormatType::kUnknown};
    // Load and store ops are dynamic (in render pass via render begin command)
//    RHILoadOpType load_op {RHILoadOpType::kClear};
//    RHIStoreOpType store_op {RHIStoreOpType::kStore};
};

struct RHIDepthStencilAttachmentDesc {
    // Unknown for no depth stencil attachment
    PixelFormatType format {PixelFormatType::kUnknown};
    // Load and store ops are dynamic (in render pass via render begin command)
    // RHILoadOpType load_op;
    // RHIStoreOpType store_op;
};

struct RHIGraphicsPipelineDesc {
    struct {
        RHIShader * vertex_shader {};
        RHIShader * fragment_shader {};
        RHIShader * geometry_shader {};
        RHIShader * task_shader {};
        RHIShader * mesh_shader {};
    } stages;
    struct {
        std::span<RHIVertexInputBindingDesc> vertex_buffers;
        std::span<RHIVertexInputAttributeDesc> vertex_attributes;
    } vertex_input;
    // Draw topology
    RHIPrimitiveTopologyType topology {RHIPrimitiveTopologyType::kTriangleList};
    struct {
        // Enable depth test
        bool depth_test_enable {false};
        // Enable depth write
        bool depth_write_enable {false};
        // Depth comparison function
        RHIDepthCompareOpType depth_compare_op {RHIDepthCompareOpType::kLess};
    } depth_stencil;
    std::span<RHIColorAttachmentDesc> color_attachments;
    RHIDepthStencilAttachmentDesc depth_stencil_attachment;
};

struct RHITextureDimensions {
    uint32_t width {0};
    uint32_t height {0};
    uint32_t depth {1};
};

struct RHITextureDesc {
    RHITextureType type;
    RHITextureDimensions dimensions;
    uint32_t mip_levels {1};
    uint32_t array_layers {1};
    PixelFormatType format {PixelFormatType::kUnknown};
    RHITextureUsageFlags usage {};
};

struct RHIPipelineParameterBufferDesc {
    RHIBufferSpan buffer;
    // On which resource slot to bind the resource
    uint32_t slot;
};
struct RHIPipelineParameterTextureDesc {
    RHITexture * texture;
    // On which resource slot to bind the resource
    uint32_t slot;
};
struct RHIPipelineParameterResourceDesc {
    RHIResource * resource;
    // On which resource slot to bind the resource
    uint32_t slot;
};

struct RHIBindPipelineParametersDesc {
    // Points to a segment of temporary memory allocated through the command buffer.
    std::span<RHIPipelineParameterBufferDesc> uniforms {};
    std::span<RHIPipelineParameterBufferDesc> storages {};
    std::span<RHIPipelineParameterTextureDesc> uavs {};
    std::span<RHIPipelineParameterTextureDesc> srvs {};
    std::span<RHIPipelineParameterResourceDesc> samplers {};
    std::span<RHIPipelineParameterResourceDesc> acceleration_structures {};
    // Constants, null for do-not-set. Allocate this memory through the command buffer.
    std::span<std::byte> constants {};
};

struct ShaderVertexInputDesc {
    RHIVertexAttributeFormatType format;
    uint32_t name_crc;
    uint32_t location;
    std::string name;
};

struct ShaderFragmentOutputDesc {
    RHIFragmentOutputFormatType format;
    uint32_t name_crc;
    uint32_t location;
    std::string name;
};

namespace PipelineReflection {
    struct UniformBufferDesc {
        uint32_t size;
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        // Deep reflection into constant buffer structs in the shader
        RHIParamStructInfo * struct_reflection;
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array ofunspecified length.
        uint32_t array_size;
    };
    struct StorageBufferDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        RHIGPUAccessFlags access_flags;
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array ofunspecified length.
        uint32_t array_size;
    };
    struct UAVDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array ofunspecified length.
        uint32_t array_size;
    };
    struct SRVDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array ofunspecified length.
        uint32_t array_size;
    };
    struct SamplerDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
    };
    typedef SamplerDesc ImmutableSamplerDesc;
    struct AccelerationStructureDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array ofunspecified length.
        uint32_t array_size;
    };
    struct CommandConstantDesc {
        uint32_t size;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
    };
}

namespace ShaderReflection {
    // Specify the offset of the decorations of a particular resource within the shader IR.
    struct IRBindingDecorationLocation {
        uint32_t binding_offset; // Word offset of the resource binding number
        uint32_t set_offset;     // Word offset of the resource set number
    };
    struct UniformBufferDesc {
        IRBindingDecorationLocation locations;
        uint32_t size;
        uint32_t name_crc;
        // TODO reflection into uniform buffer structs
        std::string name;
        // Deep reflection into constant buffer structs in the shader
        RHIParamStructInfo * struct_reflection;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::UniformBufferDesc ToPipelineDesc() const {
            return {
                size, name_crc, 0, name,
                // FIXME Here we directly shares the memory among RHIShader's reflection and RHIPipeline's reflection.
                // The current implementation simply leaks all memory for shader struct reflection.
                // If we further implemented proper memory recycling, this may cause a floating pointer error.
                struct_reflection,
                array_size
            };
        }
    };
    struct StorageBufferDesc {
        IRBindingDecorationLocation locations;
        uint32_t name_crc;
        std::string name;
        RHIGPUAccessFlags access_flags;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::StorageBufferDesc ToPipelineDesc() const {
            return {name_crc, 0, name, access_flags, array_size};
        }
    };
    struct UAVDesc {
        IRBindingDecorationLocation locations;
        uint32_t name_crc;
        std::string name;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::UAVDesc ToPipelineDesc() const {
            return {name_crc, 0, name, array_size};
        }
    };
    struct SRVDesc {
        IRBindingDecorationLocation locations;
        uint32_t name_crc;
        std::string name;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::SRVDesc ToPipelineDesc() const {
            return {name_crc, 0, name, array_size};
        }
    };
    struct SamplerDesc {
        IRBindingDecorationLocation locations;
        uint32_t name_crc;
        std::string name;
        FORCEINLINE PipelineReflection::SamplerDesc ToPipelineDesc() const {
            return {name_crc, 0, name};
        }
    };
    typedef SamplerDesc ImmutableSamplerDesc;
    struct AccelerationStructureDesc {
        IRBindingDecorationLocation locations;
        uint32_t name_crc;
        std::string name;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::AccelerationStructureDesc ToPipelineDesc() const {
            return {name_crc, 0, name, array_size};
        }
    };
    struct CommandConstantDesc {
        IRBindingDecorationLocation locations;
        uint32_t size;
        std::string name;
        FORCEINLINE PipelineReflection::CommandConstantDesc ToPipelineDesc() const {
            return {size, 0, name};
        }
    };
}

struct RHIDrawDesc {
    struct {
        float x {}, y {};
        float width {}, height {};
        float min_depth {}, max_depth {1.f};
    } viewport;
    struct {
        int x {}, y {};
        uint32_t width {}, height{};
    } scissor;
    // Framebuffer attachment count
    uint32_t num_framebuffer_attachments_ {};
    // Framebuffer attachment clear values
    std::array<float, 4> clear_values[C::kRHIMaxNumFramebufferAttachments] {};
    // Framebuffer attachments
    RHITexture * attachments[C::kRHIMaxNumFramebufferAttachments] {};
    // Framebuffer attachment load ops
    RHILoadOpType load_ops[C::kRHIMaxNumFramebufferAttachments] {};
    // Framebuffer attachment store ops
    RHIStoreOpType store_ops[C::kRHIMaxNumFramebufferAttachments] {};

    RHITexture * depth_stencil_attachment {};
    std::array<float, 4> depth_stencil_clear_value {};
    RHILoadOpType depth_stencil_load_op {};
    RHIStoreOpType depth_stencil_store_op {};

    FORCEINLINE void SetClearValues(std::array<float, 4> in_clear_values[C::kRHIMaxNumFramebufferAttachments]) {
        for(int i = 0; i < C::kRHIMaxNumFramebufferAttachments; ++i) {
            clear_values[i] = in_clear_values[i];
        }
    }
    template<typename...T>
    FORCEINLINE void SetClearValues(T...args) {
        std::array<float, 4> in_clear_values[sizeof...(args)] = {args...};
        std::copy(in_clear_values, in_clear_values + sizeof...(args), clear_values);
        for(int i = sizeof...(args); i < C::kRHIMaxNumFramebufferAttachments; ++i) {
            clear_values[i] = {0, 0, 0, 1};
        }
    }
    FORCEINLINE void SetClearValue (uint32_t index, std::array<float, 4> in_clear_value) {
        clear_values[index] = in_clear_value;
    }
    FORCEINLINE void Reset () {
        num_framebuffer_attachments_ = 0;
        viewport = {};
        scissor = {};
        for(auto & v : clear_values) {
            v=  {0, 0, 0, 1};
        }
        for(auto & ld : load_ops) {
            ld = RHILoadOpType::kClear;
        }
        for(auto & st : store_ops) {
            st = RHIStoreOpType::kStore;
        }
        for(auto & att : attachments) {
            att = nullptr;
        }
    }
    FORCEINLINE void SetNumAttachments (uint32_t num) {
        num_framebuffer_attachments_ = num;
    }
    FORCEINLINE void SetAttachment(uint32_t index, RHITexture * texture,
                                   RHILoadOpType load_op = RHILoadOpType::kClear,
                                   RHIStoreOpType store_op = RHIStoreOpType::kStore,
                                   std::array<float, 4> clear_value = {0, 0, 0, 1}) {
        attachments[index] = texture;
        load_ops[index] = load_op;
        store_ops[index] = store_op;
        num_framebuffer_attachments_ = std::max(num_framebuffer_attachments_, index + 1);
        clear_values[index] = clear_value;
    }
    FORCEINLINE void SetDepthStencilAttachment (RHITexture * texture,
        RHILoadOpType load_op = RHILoadOpType::kClear,
        RHIStoreOpType store_op = RHIStoreOpType::kStore,
        std::array<float, 4> clear_value = {0, 0, 0, 1}) {
        depth_stencil_attachment = texture;
        depth_stencil_load_op = load_op;
        depth_stencil_store_op = store_op;
        depth_stencil_clear_value = clear_value;
    }
    FORCEINLINE void SetDepthStencilClearValue (std::array<float, 4> clear_value) {
        depth_stencil_clear_value = clear_value;
    }
};

#define USE_SHADER_REFLECTION_STRUCTS using UniformBufferDesc = ShaderReflection::UniformBufferDesc; \
using StorageBufferDesc = ShaderReflection::StorageBufferDesc;                                       \
using UAVDesc = ShaderReflection::UAVDesc;                                                           \
using SRVDesc = ShaderReflection::SRVDesc;                                                           \
using SamplerDesc = ShaderReflection::SamplerDesc;                                                   \
using ImmutableSamplerDesc = ShaderReflection::ImmutableSamplerDesc;                                 \
using AccelerationStructureDesc = ShaderReflection::AccelerationStructureDesc;                       \
using CommandConstantDesc = ShaderReflection::CommandConstantDesc;

#define USE_PIPELINE_REFLECTION_STRUCTS using UniformBufferDesc = PipelineReflection::UniformBufferDesc; \
using StorageBufferDesc = PipelineReflection::StorageBufferDesc;                                       \
using UAVDesc = PipelineReflection::UAVDesc;                                                           \
using SRVDesc = PipelineReflection::SRVDesc;                                                           \
using SamplerDesc = PipelineReflection::SamplerDesc;                                                   \
using ImmutableSamplerDesc = PipelineReflection::ImmutableSamplerDesc;                                 \
using AccelerationStructureDesc = PipelineReflection::AccelerationStructureDesc;                       \
using CommandConstantDesc = PipelineReflection::CommandConstantDesc;


struct RHIDrawIndirectCommand {
    uint32_t vertex_count {};
    uint32_t instance_count {};
    uint32_t first_vertex {};
    uint32_t first_instance {};
};

struct RHIDrawIndexedIndirectCommand {
    uint32_t index_count {};
    uint32_t instance_count {};
    uint32_t first_index {};
    uint32_t vertex_offset {};
    uint32_t first_instance {};
    uint32_t padding0 {};
    uint32_t padding1 {};
    uint32_t padding2 {};
};

MI_NAMESPACE_END

#endif //MI_RHI_DESC_H
