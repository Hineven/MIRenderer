/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_pipeline.h"
#include "core/crc.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

template<typename T1, typename T2, typename = void> struct CheckSize {
    CheckSize ([[maybe_unused]] T1 val, [[maybe_unused]] T2 op)  {}
    bool operator()() {return true;}
};
template<typename T1, typename T2> struct CheckSize<T1, T2, decltype(std::declval<T1>()->val)> {
    T1 val;
    T2 op;
    CheckSize (T1 val, T2 op) : val(val), op(op) {}
    bool operator()() {return val->size == op->size;}
};

// Remap shader resources to pipeline resources, merge shader resources with the same name
// and do some simple consistency validation.
bool RHIPipeline::CheckAndRemapShaderResources(RHIShader *shader) {
    // Omit empty shaders.
    if(!shader) return true;
    // Gather & align shader slots to pipeline slots
    auto GatherShaderSlots = [&]<RHIPipelineResourceType Type, typename T1, typename T2>(const IVector<T1> & resource_descs, IVector<T2> & pipeline_resource_descs) {
        for(int i = 0; i < resource_descs.size(); ++i) {
            int pipeline_slot;
            for(pipeline_slot = 0; pipeline_slot < pipeline_resource_descs.size(); ++pipeline_slot) {
                if(pipeline_resource_descs[pipeline_slot].name_crc == resource_descs[i].name_crc) {
                    break;
                }
            }
            if(pipeline_slot == pipeline_resource_descs.size()) {
                // Assign new slot
                auto & desc = pipeline_resource_descs.emplace_back(resource_descs[i].ToPipelineDesc());
                desc.frequency_bits = desc.frequency_bits | shader->GetFrequency();
            } else {
                // Check their sizes if possible
                bool sizes_matched = CheckSize(&resource_descs[i], &pipeline_resource_descs[pipeline_slot])();
                if(!sizes_matched) {
                    MI_LOG(MIInfraLogType::kWarning, "Resource sizes mismatch");
                    return false;
                }
                // Mark usage
                pipeline_resource_descs[pipeline_slot].frequency_bits
                    = pipeline_resource_descs[pipeline_slot].frequency_bits | shader->GetFrequency();
            }
        }
        return true;
    };
    if (!GatherShaderSlots.operator()<RHIPipelineResourceType::kUniformBuffer>(
            shader->GetUniformBufferDesc(), uniform_buffers_))
        return false;
    if (!GatherShaderSlots.operator()<RHIPipelineResourceType::kStorageBuffer>(shader->GetStorageBufferDesc(), storage_buffers_))
        return false;
    if (!GatherShaderSlots.operator()<RHIPipelineResourceType::kUAV>(shader->GetUAVDesc(), uavs_)) return false;
    if (!GatherShaderSlots.operator()<RHIPipelineResourceType::kSRV>(shader->GetSRVDesc(), srvs_)) return false;
    if (!GatherShaderSlots.operator()<RHIPipelineResourceType::kSampler>(shader->GetSamplerDesc(), samplers_)) return false;
    if (!GatherShaderSlots.operator()<RHIPipelineResourceType::kImmutableSampler>(
            shader->GetImmutableSamplerDesc(), immutable_samplers_))
        return false;
    if(!GatherShaderSlots.operator()<RHIPipelineResourceType::kAccelerationStructure>(
            shader->GetAccelerationStructureDesc(), acceleration_structures_))
        return false;

    // Check command constants
    if (shader->HasCommandConstant()) {
        if(command_constant_.empty()) {
            command_constant_.push_back(shader->GetCommandConstantDesc().ToPipelineDesc());
        } else {
            if(command_constant_[0].size != shader->GetCommandConstantDesc().size) {
                MI_LOG(MIInfraLogType::kWarning, "Command constant sizes mismatch");
                return false;
            }
        }
    }

    return true;
}

bool RHIPipeline::CheckNoOverlappingNamesAmongDifferentTypes() {
    IVector<uint32_t> name_crc;
    auto Inject = [&] (const auto & arr) {
        for(const auto & desc : arr) {
            name_crc.push_back(desc.name_crc);
        }
    };
    Inject(uniform_buffers_);
    Inject(storage_buffers_);
    Inject(uavs_);
    Inject(srvs_);
    Inject(samplers_);
    Inject(immutable_samplers_);
    Inject(acceleration_structures_);
    std::sort(name_crc.begin(), name_crc.end());
    for(int i = 1; i < name_crc.size(); ++i) {
        if(name_crc[i] == name_crc[i - 1]) {
            MI_LOG(MIInfraLogType::kWarning, "Resource names overlap among different shaders.");
            return false;
        }
    }
    return true;
}

void RHIPipeline::TryLocateAndStripBindlessTableUniformBuffer() {
#define TO_STR_IMPL(x) #x
#define STR(x) TO_STR_IMPL(x)
    static auto bindless_table_name = STR(BINDLESS_TABLE_UNIFORM_BUFFER_NAME);
    uint32_t bindless_table_name_crc = CRC32(bindless_table_name, strlen(bindless_table_name));
    has_bindless_resources_ = false;
    for(int i = 0; i < uniform_buffers_.size(); ++i) {
        if(uniform_buffers_[i].name_crc == bindless_table_name_crc) {
            has_bindless_resources_ = true;
            bindless_table_size_ = uniform_buffers_[i].size;
            // Strip the bindless table uniform buffer from the pipeline resources
            uniform_buffers_.erase(uniform_buffers_.begin() + i);
            return;
        }
    }

}

void RHIPipeline::Reset() {
    uniform_buffers_.clear();
    storage_buffers_.clear();
    uavs_.clear();
    srvs_.clear();
    samplers_.clear();
    immutable_samplers_.clear();
    acceleration_structures_.clear();
    command_constant_.clear();
    has_bindless_resources_ = false;
    bindless_table_size_ = 0;
    is_valid_ = false;
    ResetRHI();
}

void RHIGraphicsPipeline::Compile(const RHIGraphicsPipelineDesc & desc) {
    Reset();
    if(!CheckAndRemapShaderResources(desc.stages.vertex_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.fragment_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.geometry_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.mesh_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.task_shader)) return;
    if(!CheckNoOverlappingNamesAmongDifferentTypes()) return;
    TryLocateAndStripBindlessTableUniformBuffer();

    vertex_inputs_    = desc.stages.vertex_shader->GetVertexInputDesc();
    fragment_outputs_ = desc.stages.fragment_shader->GetFragmentOutputDesc();
    if(desc.color_attachments.size() != fragment_outputs_.size()) {
        MI_LOG(MIInfraLogType::kWarning, "Color attachment count mismatch");
        return ;
    }
    for(int i = 0; i < desc.color_attachments.size(); ++i) {
        if(fragment_outputs_[i].format == RHIFragmentOutputFormatType::k4xFp32) {
            if(desc.color_attachments[i].format != PixelFormatType::kR16G16B16A16_FLOAT
            && desc.color_attachments[i].format != PixelFormatType::kR32G32B32A32_FLOAT
            && desc.color_attachments[i].format != PixelFormatType::kR16G16_FLOAT
            && desc.color_attachments[i].format != PixelFormatType::kR32G32B32_FLOAT
            && desc.color_attachments[i].format != PixelFormatType::kR32G32_FLOAT
            && desc.color_attachments[i].format != PixelFormatType::kR32_FLOAT
            && desc.color_attachments[i].format != PixelFormatType::kR8G8B8A8_UNORM
            && desc.color_attachments[i].format != PixelFormatType::kR8G8B8A8_SRGB) {
                MI_LOG(MIInfraLogType::kWarning, "Color attachment {} format mismatch", i);
                return ;
            }
        } else if(fragment_outputs_[i].format == RHIFragmentOutputFormatType::k4xUIint32) {
            if(desc.color_attachments[i].format != PixelFormatType::kR32G32B32A32_UINT
            && desc.color_attachments[i].format != PixelFormatType::kR32G32_UINT
            && desc.color_attachments[i].format != PixelFormatType::kR32_UINT) {
                MI_LOG(MIInfraLogType::kWarning, "Color attachment {} format mismatch", i);
                return ;
            }
        }
    }
    if(desc.depth_stencil_attachment.format != PixelFormatType::kD32_FLOAT) {
        MI_LOG(MIInfraLogType::kWarning, "Depth stencil attachment format mismatch");
        return ;
    }

    if(!CompileRHI(desc)) return;
    is_valid_ = true;
}

void RHIGraphicsPipeline::Reset() {
    vertex_inputs_.clear();
    fragment_outputs_.clear();
    RHIPipeline::Reset();
}

void RHIComputePipeline::Compile(mi::RHIShader *compute_shader) {
    Reset();
    if(!CheckAndRemapShaderResources(compute_shader)) return;
    if(!CheckNoOverlappingNamesAmongDifferentTypes()) return;
    TryLocateAndStripBindlessTableUniformBuffer();
    if(!CompileRHI(compute_shader)) return;
    is_valid_ = true;
}


MI_NAMESPACE_END