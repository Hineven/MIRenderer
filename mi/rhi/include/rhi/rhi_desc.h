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
#include <vector>
#include <glm/detail/qualifier.hpp>

#include "rhi/rhi_fwd.h"
#include "rhi_types.h"
#include "core/pixel_format.h"
#include "core/constants.h"

MI_NAMESPACE_BEGIN

struct RHIDeviceProperties {
    uint32_t wave_size {}; // Wave size in threads
    char device_name[256] {}; // Device name

    // Ray tracing properties
    uint32_t shader_group_handle_size {};         // Size of a shader group handle
    uint32_t shader_group_handle_alignment {};    // Alignment of shader group handles
    uint32_t shader_group_base_alignment {};      // Base alignment for SBT entries
    uint32_t max_ray_recursion_depth {};          // Maximum ray recursion depth
    uint32_t max_shader_group_stride {};          // Maximum stride for shader binding table

    // Partitioned acceleration structure (VK_NV_partitioned_acceleration_structure) properties
    uint32_t max_partition_count {};              // Maximum number of partitions in a PTLAS

    float    timestamp_period {};            // Timestamp period in nanoseconds (1 timestamp = this many ns)
    uint32_t timestamp_valid_bits {};        // Number of valid bits in a timestamp value
};

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
    // uint32_t descriptor_buffer_offset_alignment;
};

// Describes the layout of a root signature (pipeline layout) to be created via RHI::CreateRootSignature.
// This is the RHI-level contract: it specifies how many resources of each type the pipeline expects,
// their name CRCs for binding remapping, whether bindless resources are used, and the push constant size.
// The RHI backend (e.g. Vulkan) uses this to create the underlying pipeline layout and descriptor set layouts.
// Multiple pipelines can share the same root signature if they declare compatible resource layouts.
struct RHIPipelineRootSignatureDesc {
    uint32_t push_constant_size {};

    uint32_t num_resources[(uint32_t)RHIPipelineResourceType::kMax] {};

    struct TypeNames {
        const uint32_t * name_crcs {};
        uint32_t count {};
    };
    TypeNames type_names[(uint32_t)RHIPipelineResourceType::kMax] {};
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
    // Enable this flag to specify no rasterization. Running only the vertex processing stages.
    bool rasterization_discard {false};
    std::span<RHIColorAttachmentDesc> color_attachments;
    RHIDepthStencilAttachmentDesc depth_stencil_attachment;
};

// Ray tracing shader group description
struct RHIRayTracingShaderGroupDesc {
    RHIRayTracingShaderGroupType type {RHIRayTracingShaderGroupType::kMax};
    uint32_t general_shader_index {UINT32_MAX};      // Index for raygen, miss, or callable shaders
    uint32_t closest_hit_shader_index {UINT32_MAX};  // Index for closest hit shader (hit groups only)
    uint32_t any_hit_shader_index {UINT32_MAX};      // Index for any hit shader (hit groups only)
    uint32_t intersection_shader_index {UINT32_MAX}; // Index for intersection shader (procedural hit groups only)
};

// Ray tracing pipeline description
struct RHIRayTracingPipelineDesc {
    std::vector<RHIShader*> shaders;                        // All shaders used in the pipeline
    std::vector<RHIRayTracingShaderGroupDesc> shader_groups;// Shader group descriptions
    uint32_t max_recursion_depth;                           // Maximum ray recursion depth
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

struct RHIPipelineParameterBufferArrayDesc {
    std::span<RHIBufferSpan> buffer_array;
    // On which resource slot to bind the resource
    uint32_t slot;
};

struct RHIPipelineParameterTextureDesc {
    RHITexture * texture;
    // On which resource slot to bind the resource
    uint32_t slot;
    // Array layer (if the texture is layered)
    // UINT_MAX for all layers
    uint32_t array_layer {UINT_MAX};
    // Mip level (if the texture is mipmapped)
    uint32_t mip_level {0};
    // Explicit image layout for the descriptor write.
    // If kUndefined, the backend will use its default behavior.
    RHITextureLayoutType layout {RHITextureLayoutType::kUndefined};
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
    // TODO add support for storage arrays
    // std::span<RHIPipelineParameterBufferArrayDesc> storage_arrays {};
    std::span<RHIPipelineParameterTextureDesc> uavs {};
    std::span<RHIPipelineParameterTextureDesc> srvs {};
    std::span<RHIPipelineParameterResourceDesc> samplers {};
    std::span<RHIPipelineParameterResourceDesc> acceleration_structures {};
    std::span<RHIPipelineParameterResourceDesc> partitioned_acceleration_structures {};
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
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array of unspecified length.
        uint32_t array_size;
    };
    struct StorageBufferDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        RHIGPUAccessFlags access_flags;
        // Array size of an array of resources. 0 if not an array. UINT32_MAX for array of unspecified length.
        uint32_t array_size;
    };
    struct UAVDesc {
        uint32_t name_crc;
        // Stages in which the resource is available
        RHIShaderFrequencyFlags frequency_bits;
        std::string name;
        RHIGPUAccessFlags access_flags;
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
    struct PushConstantDesc {
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
        std::string name;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::UniformBufferDesc ToPipelineDesc() const {
            return {
                size, name_crc, 0, name, array_size
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
        RHIGPUAccessFlags access_flags;
        uint32_t array_size;
        FORCEINLINE PipelineReflection::UAVDesc ToPipelineDesc() const {
            return {name_crc, 0, name,  access_flags, array_size};
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
    struct PushConstantDesc {
        IRBindingDecorationLocation locations;
        uint32_t size;
        std::string name;
        FORCEINLINE PipelineReflection::PushConstantDesc ToPipelineDesc() const {
            return {size, 0, name};
        }
    };
}

struct RHIDrawStateDesc {
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
    // Layer index for each framebuffer attachment to draw to
    uint32_t layers[C::kRHIMaxNumFramebufferAttachments] {};


    RHITexture * depth_stencil_attachment {};
    std::array<float, 4> depth_stencil_clear_value {};
    RHILoadOpType depth_stencil_load_op {};
    RHIStoreOpType depth_stencil_store_op {};

    RHIPolygonModeType polygon_mode {};

    float line_width {1.f};

    RHICullModeType cull_mode {};

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
        cull_mode = {};
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
    // For uint textures, use std::bit_cast<float>(uint_value) to set the clear value for the texture.
    FORCEINLINE void SetAttachment(uint32_t index, RHITexture * texture,
                                   RHILoadOpType load_op = RHILoadOpType::kClear,
                                   RHIStoreOpType store_op = RHIStoreOpType::kStore,
                                   std::array<float, 4> clear_value = {0, 0, 0, 1},
                                   uint32_t layer = 0) {
        attachments[index] = texture;
        load_ops[index] = load_op;
        store_ops[index] = store_op;
        num_framebuffer_attachments_ = std::max(num_framebuffer_attachments_, index + 1);
        clear_values[index] = clear_value;
        layers[index] = layer;
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
using PushConstantDesc = ShaderReflection::PushConstantDesc;

#define USE_PIPELINE_REFLECTION_STRUCTS using UniformBufferDesc = PipelineReflection::UniformBufferDesc; \
using StorageBufferDesc = PipelineReflection::StorageBufferDesc;                                       \
using UAVDesc = PipelineReflection::UAVDesc;                                                           \
using SRVDesc = PipelineReflection::SRVDesc;                                                           \
using SamplerDesc = PipelineReflection::SamplerDesc;                                                   \
using ImmutableSamplerDesc = PipelineReflection::ImmutableSamplerDesc;                                 \
using AccelerationStructureDesc = PipelineReflection::AccelerationStructureDesc;                       \
using PushConstantDesc = PipelineReflection::PushConstantDesc;


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

struct RHIDispatchIndirectCommand {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t padding;
};

struct RHITraceRaysIndirectCommand {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint32_t padding;
};

struct RHITraceRaysIndirectCommand2 {
    uint32_t x;
    uint32_t y;
    uint32_t z;
    uint64_t raygen_sbt_address {UINT64_MAX};
    uint64_t raygen_sbt_size {UINT64_MAX};
    uint64_t miss_sbt_address {UINT64_MAX};
    uint64_t miss_sbt_size {UINT64_MAX};
    uint64_t miss_sbt_stride {UINT64_MAX};
    uint64_t hit_sbt_address {UINT64_MAX};
    uint64_t hit_sbt_size {UINT64_MAX};
    uint64_t hit_sbt_stride {UINT64_MAX};
    uint64_t callable_sbt_address {UINT64_MAX};
    uint64_t callable_sbt_size {UINT64_MAX};
    uint64_t callable_sbt_stride {UINT64_MAX};
};

struct RHISamplerDesc {
    RHISamplerFilterType min_filter {RHISamplerFilterType::kLinear};
    RHISamplerFilterType mag_filter {RHISamplerFilterType::kLinear};
    RHISamplerFilterType mipmap_mode {RHISamplerFilterType::kLinear};
    RHISamplerAddressModeType address_mode_u {RHISamplerAddressModeType::kRepeat};
    RHISamplerAddressModeType address_mode_v {RHISamplerAddressModeType::kRepeat};
    RHISamplerAddressModeType address_mode_w {RHISamplerAddressModeType::kRepeat};
    std::array<float, 4> border_color {0.f, 0.f, 0.f, 1.f};
};

MI_NAMESPACE_END

#endif //MI_RHI_DESC_H
