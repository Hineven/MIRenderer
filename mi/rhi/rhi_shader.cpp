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
            T desc;
            desc.name = resource.name;
            if constexpr (THasSize<T>::value) {
                desc.size = (uint32_t)compiler_hlsl.get_declared_struct_size(compiler_hlsl.get_type(resource.base_type_id));
            }
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationBinding, desc.locations.binding_offset);
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationDescriptorSet, desc.locations.set_offset);
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            out_resources.push_back(desc);
        }
    };
    ReflectResources.operator()<UniformBufferDesc>(shader_resources.uniform_buffers, uniform_buffers_with_bindless_table_);
    // Deep reflection of the uniform buffers. Reveal their underlying structures.
    {
        auto IsBasicType = [](spirv_cross::SPIRType::BaseType t) {
            if (
                t == spirv_cross::SPIRType::Image || t == spirv_cross::SPIRType::Sampler || t == spirv_cross::SPIRType::SampledImage
                || t == spirv_cross::SPIRType::AccelerationStructure || t == spirv_cross::SPIRType::Struct
            ) {
                return false;
            }
            if (
                t == spirv_cross::SPIRType::Float || t == spirv_cross::SPIRType::Int || t == spirv_cross::SPIRType::UInt) {
                return true;
                }
            assert(false && "Not Implemented");
        };
        auto GetBasicParamType = [](spirv_cross::SPIRType::BaseType t, uint32_t vec_size) {
            if (t == spirv_cross::SPIRType::Float) {
                if (vec_size == 1) {
                    return RHIBasicParamType::kFloat;
                } else if (vec_size == 2) {
                    return RHIBasicParamType::kFloat2;
                } else if (vec_size == 3) {
                    return RHIBasicParamType::kFloat3;
                } else if (vec_size == 4) {
                    return RHIBasicParamType::kFloat4;
                }
            } else if (t == spirv_cross::SPIRType::Int) {
                if (vec_size == 1) {
                    return RHIBasicParamType::kInt;
                } else if (vec_size == 2) {
                    return RHIBasicParamType::kInt2;
                } else if (vec_size == 3) {
                    return RHIBasicParamType::kInt3;
                } else if (vec_size == 4) {
                    return RHIBasicParamType::kInt4;
                }
            } else if (t == spirv_cross::SPIRType::UInt) {
                if (vec_size == 1) {
                    return RHIBasicParamType::kUInt;
                } else if (vec_size == 2) {
                    return RHIBasicParamType::kUInt2;
                } else if (vec_size == 3) {
                    return RHIBasicParamType::kUInt3;
                } else if (vec_size == 4) {
                    return RHIBasicParamType::kUInt4;
                }
            }
            assert(false && "Not Implemented");
            return RHIBasicParamType::kMax;
        };
        std::function<RHIParamStructInfo*(spirv_cross::TypeID)> RecursiveDeepReflection = [&] (spirv_cross::TypeID reflecting_type_id) {
            RHIParamStructInfo * ret = new RHIParamStructInfo;
            spirv_cross::SPIRType type = compiler_hlsl.get_type(reflecting_type_id);
            std::vector<RHIParamInfo> reflected_members;
            for (auto const & [i, member] : type.member_types | std::views::enumerate) {
                spirv_cross::SPIRType member_type = compiler_hlsl.get_type(member);
                RHIParamInfo info {};
                info.name = compiler_hlsl.get_member_name(reflecting_type_id, member);
                info.size = compiler_hlsl.get_declared_struct_size(member_type);
                info.offset = compiler_hlsl.type_struct_member_offset(type, i);
                if (IsBasicType(member_type.basetype)) {
                    // Stop recursion
                    info.type = RHIParamType::kBasic;
                    info.basic_type = GetBasicParamType(member_type.basetype, member_type.vecsize);
                } else {
                    // There are only structs in uniform buffers
                    if (member_type.basetype != spirv_cross::SPIRType::Struct) {
                        assert(false && "Invalid uniform buffer. Constant buffers should not contain shader resources.");
                    }
                    info.type = RHIParamType::kStruct;
                    info.struct_info = RecursiveDeepReflection(member);
                }
                reflected_members.push_back(info);
            }
            auto members_mem = new RHIParamInfo[reflected_members.size()];
            std::copy(reflected_members.begin(), reflected_members.end(), members_mem);
            ret->members = byte_strided_span((RHIParamInfo*)members_mem, reflected_members.size(), sizeof(RHIParamInfo));
            ret->InitializeLayoutHash();
            return ret;
        };
        for (const auto& [i, compiler_resource]: std::views::enumerate(shader_resources.uniform_buffers)) {
            auto& desc = uniform_buffers_with_bindless_table_[i];
            const auto& type = compiler_hlsl.get_type(compiler_resource.base_type_id);
            if (type.basetype != spirv_cross::SPIRType::Struct) {
                assert(false && "Invalid uniform buffer. Constant buffers should always be structs.");
            }
            desc.struct_reflection = RecursiveDeepReflection(compiler_resource.base_type_id);
        }
    }
    // Manually reflect storage buffers to separate RW / R only buffers
    {
        for (auto & resource : shader_resources.storage_buffers) {
            StorageBufferDesc desc;
            desc.name = resource.name;
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationBinding, desc.locations.binding_offset);
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationDescriptorSet, desc.locations.set_offset);
            uint32_t word_offset;
            bool has_decoration = compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationNonWritable, word_offset);
            bool read_only = has_decoration && (bool)(((uint32_t*)ir_)[word_offset]);
            desc.access_flags = {};
            if (!read_only) desc.access_flags = desc.access_flags | RHIGPUAccessFlagBits::kWrite;
            desc.access_flags = desc.access_flags | RHIGPUAccessFlagBits::kRead;
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            storage_buffers_.push_back(desc);
        }
    }
    ReflectResources.operator()<UAVDesc>( shader_resources.storage_images, uavs_);
    ReflectResources.operator()<SRVDesc>( shader_resources.separate_images, srvs_);
    ReflectResources.operator()<SamplerDesc>( shader_resources.separate_samplers, samplers_);
    // Immutable samplers are not supported for now (and they can not be really reflected from SPIRV)
//    ReflectResources.operator()<SamplerDesc>( shader_resources.separate_samplers, immutable_samplers_);
    ReflectResources.operator()<AccelerationStructureDesc>( shader_resources.acceleration_structures, acceleration_structures_);
    for (auto & resource : shader_resources.push_constant_buffers) {
        CommandConstantDesc desc;
        desc.name = resource.name;
        desc.size = (uint32_t)compiler_hlsl.get_declared_struct_size(compiler_hlsl.get_type(resource.base_type_id));
        command_constant_.push_back(desc);
    }
    // Look for bindless table uniform buffer
    int bindless_table_index = 0;
    for(auto & uniforms : uniform_buffers_with_bindless_table_) {
        if(uniforms.name == TO_STR(BINDLESS_TABLE_UNIFORM_BUFFER_NAME)) {
            break;
        }
        bindless_table_index ++;
    }
    has_bindless_resources_ = bindless_table_index != uniform_buffers_with_bindless_table_.size();
    bindless_table_uniform_index_ = bindless_table_index;


    // Reflect shader inputs & outputs
    if(frequency_ == RHIShaderFrequencyFlagBits::kVertex) {
        // Vertex shader inputs
        // Position, COLOR0, ...
        const auto & inputs = compiler_hlsl.get_shader_resources().stage_inputs;
        for(auto & input : inputs) {
            ShaderVertexInputDesc desc;
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
        // SV_Target0, SV_Target1, ...
        const auto & outputs = compiler_hlsl.get_shader_resources().stage_outputs;
        for(auto & output : outputs) {
            ShaderFragmentOutputDesc desc;
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
            return FindResourceIndexImpl.operator()<UniformBufferDesc>(uniform_buffers_with_bindless_table_);
        case RHIPipelineResourceType::kStorageBuffer:
            return FindResourceIndexImpl.operator()<StorageBufferDesc>(storage_buffers_);
        case RHIPipelineResourceType::kUAV:
            return FindResourceIndexImpl.operator()<UAVDesc>(uavs_);
        case RHIPipelineResourceType::kSRV:
            return FindResourceIndexImpl.operator()<SRVDesc>(srvs_);
        case RHIPipelineResourceType::kSampler:
            return FindResourceIndexImpl.operator()<SamplerDesc>(samplers_);
        case RHIPipelineResourceType::kImmutableSampler:
            return FindResourceIndexImpl.operator()<ImmutableSamplerDesc>(immutable_samplers_);
        case RHIPipelineResourceType::kAccelerationStructure:
            return FindResourceIndexImpl.operator()<AccelerationStructureDesc>(acceleration_structures_);
        default: ;
    }
    return -1;
}


void RHIShader::Reset () {

    ResetRHI();

    // Clear reflection data
    uniform_buffers_with_bindless_table_.clear();
    storage_buffers_.clear();
    uavs_.clear();
    srvs_.clear();
    samplers_.clear();
    immutable_samplers_.clear();
    command_constant_.clear();
    acceleration_structures_.clear();
    has_bindless_resources_ = false;
    bindless_table_uniform_index_ = 0;
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