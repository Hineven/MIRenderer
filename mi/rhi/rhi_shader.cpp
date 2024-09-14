/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <spirv_cross/spirv_hlsl.hpp>
#include <span>

#include "core/crc.h"
#include "core/infra.h"
#include "rhi/rhi_shader.h"
#include "rhi_device_shared.h"

MI_NAMESPACE_BEGIN

#define TO_STR_IMP(VAR) #VAR
#define TO_STR(VAR) TO_STR_IMP(VAR)

RHIShader::RHIShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                     RHIShaderIRType ir_type, std::span<const std::byte> ir) {
    frequency_ = frequency;
    entry_name_ = entry_name;
    ir_type_ = ir_type;
    ir_size_ = ir.size();
    ir_ = std::make_unique<std::byte[]>(ir_size_);
    std::copy(ir.begin(), ir.end(), ir_.get());
}

RHIShader::~RHIShader() {
    Reset();
}

bool RHIShader::ReflectShaderResources() {
    mi_assert(ir_type_ == RHIShaderIRType::kSPIRV, "Only SPIRV is supported for reflection now.");
    return ReflectShaderResourcesSPIRV();
}

template<typename T, typename = void>
struct THasSize : std::false_type {};
template<typename T>
struct THasSize <T, decltype(std::declval<T>.size)> : std::true_type {};

bool RHIShader::ReflectShaderResourcesSPIRV() {
    if(ir_size_ % 4 != 0) {
        MI_LOG(MIInfraLogType::kWarning, "SPIRV IR code size must be a multiple of 4");
        return false;
    }
    // We assume that the SPIRV code is compiled from HLSL
    spirv_cross::CompilerHLSL compiler_hlsl((uint32_t*)ir_.get(), ir_size_);
    auto shader_resources = compiler_hlsl.get_shader_resources();
    auto ReflectResources =  [&] <typename T> (auto resources, auto & out_resources) {
        for (auto & resource : resources) {
            T desc;
            desc.name = resource.name;
            if constexpr (THasSize<T>::value) {
                desc.size = compiler_hlsl.get_declared_struct_size(compiler_hlsl.get_type(resource.base_type_id));
            }
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationBinding, desc.locations.binding_offset);
            compiler_hlsl.get_binary_offset_for_decoration(resource.id, spv::DecorationDescriptorSet, desc.locations.set_offset);
            desc.name_crc = CRC32(desc.name.data(), desc.name.size());
            out_resources.push_back(desc);
        }
    };
    ReflectResources.operator()<UniformBufferDesc>(shader_resources.uniform_buffers, uniform_buffers_with_bindless_table_);
    ReflectResources.operator()<StorageBufferDesc>( shader_resources.storage_buffers, storage_buffers_);
    ReflectResources.operator()<UAVDesc>( shader_resources.storage_images, uavs_);
    ReflectResources.operator()<SRVDesc>( shader_resources.separate_images, srvs_);
    ReflectResources.operator()<SamplerDesc>( shader_resources.separate_samplers, samplers_);
    // Immutable samplers are not supported for now (and they can not be really reflected from SPIRV)
//    ReflectResources.operator()<SamplerDesc>( shader_resources.separate_samplers, immutable_samplers_);
    ReflectResources.operator()<AccelerationStructureDesc>( shader_resources.acceleration_structures, acceleration_structures_);
    for (auto & resource : shader_resources.push_constant_buffers) {
        CommandConstantDesc desc;
        desc.name = resource.name;
        desc.size = compiler_hlsl.get_declared_struct_size(compiler_hlsl.get_type(resource.base_type_id));
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
    // Vertex shader inputs
    if(frequency_ == RHIShaderFrequencyFlagBits::kVertex) {
        const auto & inputs = compiler_hlsl.get_shader_resources().stage_inputs;
        for(auto & input : inputs) {
            ShaderVertexInputDesc desc;
            desc.name = input.name;
            const auto type = compiler_hlsl.get_type(input.type_id);
            if(type.pointer || type.array.size() > 0) {
                MI_LOG(MIInfraLogType::kWarning, "Pointer or array type is not supported for shader inputs.");
                return false;
            }
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
    }

    return true;
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

    is_valid_ = false;
}

void RHIShader::Compile () {
    Reset();
    if(ReflectShaderResources()) {
        if (CompileRHI()) is_valid_ = true;
        else Reset();
    } else Reset();
}

MI_NAMESPACE_END