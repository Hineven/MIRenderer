/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <span>
#include <ranges>
#include <spirv_cross/spirv_hlsl.hpp>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include "core/crc.h"
#include "core/infra.h"
#include "rhi/rhi_shader.h"

#include <rhi/rhi_param.h>

#include "rhi_device_shared.h"

MI_NAMESPACE_BEGIN

#define TO_STR_IMP(VAR) #VAR
#define TO_STR(VAR) TO_STR_IMP(VAR)

RHIShader::RHIShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                     RHIShaderIRType ir_type, std::span<const std::byte> ir) {
    frequency_ = frequency;
    entry_name_ = entry_name;
    ir_type_ = ir_type;
    ir_size_ = (uint32_t)ir.size();
    ir_ = static_cast<std::byte *>(operator new (ir_size_));
    std::copy(ir.begin(), ir.end(), ir_);
}

RHIShader::~RHIShader() {
    Reset();
    operator delete(ir_);
}

bool RHIShader::ReflectShaderResources() {
    mi_assert(ir_type_ == RHIShaderIRType::kSPIRV, "Only SPIRV is supported for reflection now.");
    return ReflectShaderResourcesSPIRV();
}

template<typename T, typename = void>
struct THasSize : std::false_type {};
template<typename T>
struct THasSize <T, std::void_t<decltype(std::declval<T>().size)>> : std::true_type {};

template<typename T, typename = void>
struct THasArraySize : std::false_type {};
template<typename T>
struct THasArraySize <T, std::void_t<decltype(std::declval<T>().array_size)>> : std::true_type {};

bool RHIShader::ReflectShaderResourcesSPIRV() {
    if(ir_size_ % 4 != 0) {
        MI_LOG(MIInfraLogType::kWarning, "SPIRV IR code size must be a multiple of 4");
        return false;
    }

    // We assume that the SPIRV code is compiled from HLSL
    spirv_cross::CompilerHLSL compiler_hlsl((uint32_t*)ir_, ir_size_ / 4);
    auto shader_resources = compiler_hlsl.get_shader_resources();
    auto ReflectResources =  [&] <typename T> (auto resources, auto & out_resources) {
        for (auto & resource : resources) {
            T desc {};
            desc.name = compiler_hlsl.get_name(resource.id);
            if constexpr (THasSize<T>::value) {
                desc.size = (uint32_t)compiler_hlsl.get_declared_struct_size(compiler_hlsl.get_type(resource.base_type_id));
            }
            if constexpr (THasArraySize<T>::value) {
                auto type = compiler_hlsl.get_type(resource.type_id);
                if (type.array.size() > 0) {
                    if (type.array.size() != 1 || !type.array_size_literal[0]) {
                        MI_WARN("Shader {}: Resource {} is multi-dimensional array ({} dimensions) or its size is not a literal."
                                "We only support 1 dimension array.", GetEntryName(), desc.name, type.array.size());
                        continue ;
                    }
                    desc.array_size = type.array[0];
                    if (!desc.array_size) desc.array_size = UINT32_MAX; // Unknown bound is reflected to 0, in our implementation we use UINT32_MAX
                } else {
                    // If the resource is not an array, we set the array size to 0.
                    desc.array_size = 0;
                }
            }
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationBinding, desc.locations.binding_offset);
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationDescriptorSet, desc.locations.set_offset);
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            out_resources.push_back(desc);
        }
    };
    ReflectResources.operator()<UniformBufferDesc>(shader_resources.uniform_buffers, uniform_buffers_);
    // 25.5.19: Removed deep reflection for uniform buffer structs for simplicity.
    // Manually reflect storage buffers to separate RW / R only buffers
    {
        for (auto & resource : shader_resources.storage_buffers) {
            StorageBufferDesc desc {};
            // desc.name = resource.name; <- this turns out to be the name of the OpTypeStruct!!!! not OpVariable!!!
            desc.name = compiler_hlsl.get_name(resource.id);
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationBinding, desc.locations.binding_offset);
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationDescriptorSet, desc.locations.set_offset);
            auto bitset = compiler_hlsl.get_decoration_bitset(resource.id);
            // 25.5.29: have to get the type of the variable for RW identification. This is a pointer to a storage buffer
            auto res_ptr_type = compiler_hlsl.get_type_from_variable(resource.id);
            // Now get the type the pointer type points to
            auto res_pointed_type_id = res_ptr_type.parent_type;
            // Okay, check if the first member of the type struct is decorated with NonWritable decoration as a member.
            // If it is, then it is a read-only storage buffer.
            bool has_decoration = compiler_hlsl.has_member_decoration(res_pointed_type_id, 0, spv::DecorationNonWritable);
            bool read_only = has_decoration;
            desc.access_flags = {};
            if (!read_only) desc.access_flags = desc.access_flags | RHIGPUAccessFlagBits::kShaderWrite;
            desc.access_flags = desc.access_flags | RHIGPUAccessFlagBits::kShaderStorageRead;
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            const auto& type = compiler_hlsl.get_type(resource.type_id);
            if (!type.array.empty()) {
                if (type.array.size() != 1 || !type.array_size_literal[0]) {
                    MI_WARN("Shader {}: Storage buffer {} is multi-dimensional array ({} dimensions) or its size is not a literal."
                            "We only support 1 dimension array.", GetEntryName(), desc.name, type.array.size());
                    continue ;
                }
                desc.array_size = type.array[0];
                if (!desc.array_size) desc.array_size = UINT32_MAX; // Unknown bound is reflected to 0, in our implementation we use UINT32_MAX
            }
            storage_buffers_.push_back(desc);
        }
    }
    ReflectResources.operator()<UAVDesc>( shader_resources.storage_images, uavs_);
    ReflectResources.operator()<SRVDesc>( shader_resources.separate_images, srvs_);
    ReflectResources.operator()<SamplerDesc>( shader_resources.separate_samplers, samplers_);
    ReflectResources.operator()<AccelerationStructureDesc>( shader_resources.acceleration_structures, acceleration_structures_);
    for (auto & resource : shader_resources.push_constant_buffers) {
        CommandConstantDesc desc;
        desc.name = resource.name;
        desc.size = (uint32_t)compiler_hlsl.get_declared_struct_size(compiler_hlsl.get_type(resource.base_type_id));
        command_constant_.push_back(desc);
    }
    // Strip bindless resource arrays
    {
        auto index = -1;
        for (auto [i, storage_buffer] : std::views::enumerate(storage_buffers_)) {
            if (storage_buffer.name == std::string(TO_STR(BINDLESS_RESOURCE_ARRAY_PREFIX)) + "Buffer") {
                if (storage_buffer.array_size != UINT32_MAX) {
                    MI_WARN("Shader {}: Bindless resource array {} should be 1D array with unspecified size.",
                            GetEntryName(), storage_buffer.name);
                    return false;
                }
                has_bindless_resources_ = true;
                index = (int)i;
                break;
            }
        }
        if (index != -1) {
            bindless_.storage_buffer = storage_buffers_[index];
            storage_buffers_.erase(storage_buffers_.begin() + index);
        }
        index = -1;
        for (auto [i, srv] : std::views::enumerate(srvs_)) {
            if (srv.name == std::string(TO_STR(BINDLESS_RESOURCE_ARRAY_PREFIX)) + "Texture") {
                if (srv.array_size != UINT32_MAX) {
                    MI_WARN("Shader {}: Bindless resource array {} should be 1D array with unspecified size.",
                            GetEntryName(), srv.name);
                    return false;
                }
                has_bindless_resources_ = true;
                index = (int)i;
                break;
            }
        }
        if (index != -1) {
            bindless_.srv = srvs_[index];
            srvs_.erase(srvs_.begin() + index);
        }
        index = -1;
        for (auto [i, as] : std::views::enumerate(acceleration_structures_)) {
            if (as.name == std::string(TO_STR(BINDLESS_RESOURCE_ARRAY_PREFIX)) + "AccelerationStructure") {
                if (as.array_size != UINT32_MAX) {
                    MI_WARN("Shader {}: Bindless resource array {} should be 1D array with unspecified size.",
                            GetEntryName(), as.name);
                    return false;
                }
                has_bindless_resources_ = true;
                index = (int)i;
                break;
            }
        }
        if (index != -1) {
            bindless_.acceleration_structure = acceleration_structures_[index];
            acceleration_structures_.erase(acceleration_structures_.begin() + index);
        }
    }

    // Index buffers and dispatch command cannot be reflected from SPIRV. They are declared in the cpp-side code
    // of RDG shaders.
    // Vertex buffers and render targets are (partially) reflected via the following code:

    // Reflect shader inputs & outputs
    if(frequency_ == RHIShaderFrequencyFlagBits::kVertex) {
        // Vertex shader inputs
        // Position, COLOR0, ...
        const auto & inputs = compiler_hlsl.get_shader_resources().stage_inputs;
        for(auto & input : inputs) {
            ShaderVertexInputDesc desc;
            // input.name: Position, COLOR0, ...
            // NOTE: it is impossible to get the real declared name in hlsl without SPV extensions (color, albedo...)
            // only the semantics name is available in the compiled spv.
            desc.name = input.name;
            // Remove the "in.var." prefix, which is generated by DXC when using SPIRV
            if(desc.name.find("in.var.") == 0) {
                desc.name = desc.name.substr(7);
            }
            const auto type = compiler_hlsl.get_type(input.type_id);
            const auto & base_type = compiler_hlsl.get_type(input.base_type_id);
            desc.location = compiler_hlsl.get_decoration(input.id, spv::DecorationLocation);
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            // determine the format
            if(base_type.basetype == spirv_cross::SPIRType::Float) {
                if(base_type.vecsize == 1) {
                    desc.format = RHIVertexAttributeFormatType::k1xFp32;
                } else if(base_type.vecsize == 2) {
                    desc.format = RHIVertexAttributeFormatType::k2xFp32;
                } else if(base_type.vecsize == 3) {
                    desc.format = RHIVertexAttributeFormatType::k3xFp32;
                } else if(base_type.vecsize == 4) {
                    desc.format = RHIVertexAttributeFormatType::k4xFp32;
                } else {
                    MI_LOG(MIInfraLogType::kWarning, "Unsupported vector size for vertex input.");
                    return false;
                }
            } else {
                // TODO support more input types
                MI_LOG(MIInfraLogType::kWarning, "Unsupported base type for vertex input.");
                return false;
            }
            vertex_inputs_.push_back(desc);
        }
    } else if(frequency_ == RHIShaderFrequencyFlagBits::kFragment) {
        // Fragment shader outputs
        const auto & outputs = compiler_hlsl.get_shader_resources().stage_outputs;
        for(auto & output : outputs) {
            ShaderFragmentOutputDesc desc;
            // output.name: SV_Target0, SV_Target1, ...
            // NOTE: it is impossible to get the real declared name in hlsl without SPV extensions (color, albedo...)
            // only the semantics name is available in the compiled spv.
            desc.name = output.name;
            // Remove the "out.var." prefix, which is generated by DXC when using SPIRV
            if(desc.name.find("out.var.") == 0) {
                desc.name = desc.name.substr(8);
            }
            const auto type = compiler_hlsl.get_type(output.type_id);
            const auto & base_type = compiler_hlsl.get_type(output.base_type_id);
            desc.location = compiler_hlsl.get_decoration(output.id, spv::DecorationLocation);
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            // determine the format
            if(base_type.basetype == spirv_cross::SPIRType::Float) {
                if(base_type.vecsize != 4) {
                    MI_LOG(MIInfraLogType::kWarning, "Unsupported vector size for fragment output.");
                    return false;
                }
                desc.format = RHIFragmentOutputFormatType::k4xFp32;
            } else if(base_type.basetype == spirv_cross::SPIRType::Int
                    ||base_type.basetype == spirv_cross::SPIRType::UInt) {
                if(base_type.vecsize != 4) {
                    MI_LOG(MIInfraLogType::kWarning, "Unsupported vector size for fragment output.");
                    return false;
                }
                desc.format = RHIFragmentOutputFormatType::k4xUIint32;
            } else {
                // TODO support more output types ?
                MI_LOG(MIInfraLogType::kWarning, "Unsupported base type for fragment output.");
                return false;
            }
            fragment_outputs_.push_back(desc);
        }
    }
    return true;
}

int RHIShader::ReflectResourceIndex(RHIPipelineResourceType type, uint32_t name_crc) const {
    auto FindResourceIndexImpl = [&] <typename T> (const auto & resources) {
        for(int i = 0; i < resources.size(); i++) {
            if(resources[i].name_crc == name_crc) {
                return i;
            }
        }
        return -1;
    };
    switch (type) {
        case RHIPipelineResourceType::kUniformBuffer:
            return FindResourceIndexImpl.operator()<UniformBufferDesc>(uniform_buffers_);
        case RHIPipelineResourceType::kStorageBuffer:
            return FindResourceIndexImpl.operator()<StorageBufferDesc>(storage_buffers_);
        case RHIPipelineResourceType::kUAV:
            return FindResourceIndexImpl.operator()<UAVDesc>(uavs_);
        case RHIPipelineResourceType::kSRV:
            return FindResourceIndexImpl.operator()<SRVDesc>(srvs_);
        case RHIPipelineResourceType::kSampler:
            return FindResourceIndexImpl.operator()<SamplerDesc>(samplers_);
        case RHIPipelineResourceType::kAccelerationStructure:
            return FindResourceIndexImpl.operator()<AccelerationStructureDesc>(acceleration_structures_);
        default:
            assert(false);
    }
    return -1;
}


void RHIShader::Reset () {

    ResetRHI();

    // Clear reflection data
    uniform_buffers_.clear();
    storage_buffers_.clear();
    uavs_.clear();
    srvs_.clear();
    samplers_.clear();
    command_constant_.clear();
    acceleration_structures_.clear();
    bindless_ = {};
    has_bindless_resources_ = false;
    vertex_inputs_.clear();
    fragment_outputs_.clear();

    is_valid_ = false;
}

void RHIShader::Compile () {
    Reset();
    if(ReflectShaderResources()) {
        if (CompileRHI()) is_valid_ = true;
        else Reset();
    } else Reset();
}


std::vector<RHIVertexInputAttributeDesc> RHIShader::GetVertexInputAttributeDescForPipeline (
        int src_binding
) const {
    mi_assert(frequency_ == RHIShaderFrequencyFlagBits::kVertex, "Only vertex shader has vertex inputs.");
    std::vector<RHIVertexInputAttributeDesc> attributes;
    uint32_t offset = 0;
    for(auto & input : vertex_inputs_) {
        RHIVertexInputAttributeDesc desc;
        desc.location = input.location;
        desc.format = input.format;
        desc.src_binding = src_binding;
        desc.offset = offset;
        offset += GetVertexAttributeFormatSize(desc.format);
        attributes.push_back(desc);
    }
    return attributes;
}

uint32_t RHIShader::GetVertexStride () const {
    mi_assert(frequency_ == RHIShaderFrequencyFlagBits::kVertex, "Only vertex shader has vertex inputs.");
    uint32_t stride = 0;
    for(auto & input : vertex_inputs_) {
        stride += GetVertexAttributeFormatSize(input.format);
    }
    return stride;
}

MI_NAMESPACE_END