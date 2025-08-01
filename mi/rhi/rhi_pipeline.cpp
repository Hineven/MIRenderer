/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <algorithm>

#include "rhi/rhi_pipeline.h"
#include "core/crc.h"
#include "core/infra.h"
#include "rhi/rhi_type_helpers.h"

MI_NAMESPACE_BEGIN

RHIPipelineResourceSlot RHIPipeline::ReflectResourceSlot(std::string_view name) const {
    return ReflectResourceSlot(CRC32(name.data(), name.size()));
}

RHIPipelineResourceSlot RHIPipeline::ReflectResourceSlot(uint32_t name_crc) const {
    return pipeline_resource_index_.at(name_crc);
}

bool RHIPipeline::HasResourceSlot(std::string_view name) const {
    return HasResourceSlot(CRC32(name.data(), name.size()));
}

bool RHIPipeline::HasResourceSlot(uint32_t name_crc) const {
    return pipeline_resource_index_.find(name_crc) != pipeline_resource_index_.end();
}

template<typename T1, typename T2, typename = void> struct CheckSize {
    CheckSize ([[maybe_unused]] T1 val, [[maybe_unused]] T2 op)  {}
    bool operator()() {return true;}
};
template<typename T1, typename T2> struct CheckSize<T1, T2, std::void_t<decltype(std::declval<T1>()->val)>> {
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
    auto GatherShaderSlots = [&]<RHIPipelineResourceType Type, typename T1, typename T2>(const std::vector<T1> & resource_descs, std::vector<T2> & pipeline_resource_descs) {
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
                desc.frequency_bits = shader->GetFrequency();
            } else {
                // Check their sizes if possible
                bool sizes_matched = CheckSize(&resource_descs[i], &pipeline_resource_descs[pipeline_slot])();
                // Check their array sizes if possible
                bool array_sizes_matched = true;
                if constexpr (requires { resource_descs[i].array_size; pipeline_resource_descs[i].array_size;}) {
                    array_sizes_matched = (resource_descs[i].array_size == pipeline_resource_descs[pipeline_slot].array_size);
                }
                if(!sizes_matched) {
                    MI_LOG(MIInfraLogType::kWarning, "Resource sizes mismatch for {}", resource_descs[i].name);
                    return false;
                }
                if (!array_sizes_matched) {
                    MI_LOG(MIInfraLogType::kWarning, "Resource array sizes mismatch for {}", resource_descs[i].name);
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
    std::vector<uint32_t> name_crc;
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

void RHIPipeline::CheckAndSetHasBindlessResources() {


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
    is_valid_ = false;
    ResetRHI();
}

static RHIGPUAccessFlags GetAccessFlags (auto elem) {
    if constexpr(requires{elem.access_flags;}) {
        return elem.access_flags;
    }
    // specially, for srv, it is always shader sampled read
    if constexpr(std::is_same_v<std::remove_cvref_t<decltype(elem)>, PipelineReflection::SRVDesc>) {
        return RHIGPUAccessFlagBits::kShaderSampledRead;
    }
    return RHIGPUAccessFlagBits::kNone;
}

void RHIPipeline::BuildPipelineResourceIndex() {
    auto Register = [&] (RHIPipelineResourceType type, const auto & arr) {
        for(int i = 0; i < arr.size(); ++i) {
            pipeline_resource_index_[arr[i].name_crc] = {
                type,
                GetAccessFlags(arr[i]),
                arr[i].frequency_bits,
                i
            };
        }
    };
    Register(RHIPipelineResourceType::kUniformBuffer, uniform_buffers_);
    Register(RHIPipelineResourceType::kStorageBuffer, storage_buffers_);
    Register(RHIPipelineResourceType::kUAV, uavs_);
    Register(RHIPipelineResourceType::kSRV, srvs_);
    Register(RHIPipelineResourceType::kSampler, samplers_);
    Register(RHIPipelineResourceType::kImmutableSampler, immutable_samplers_);
    Register(RHIPipelineResourceType::kAccelerationStructure, acceleration_structures_);
}

void RHIGraphicsPipeline::Compile(const RHIGraphicsPipelineDesc & desc) {
    Reset();
    if(!CheckAndRemapShaderResources(desc.stages.vertex_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.fragment_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.geometry_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.mesh_shader)) return;
    if(!CheckAndRemapShaderResources(desc.stages.task_shader)) return;
    if(!CheckNoOverlappingNamesAmongDifferentTypes()) return;

    has_bindless_resources_ = (desc.stages.vertex_shader && desc.stages.vertex_shader->HasBindlessResources())
                            ||(desc.stages.fragment_shader && desc.stages.fragment_shader->HasBindlessResources())
                            ||(desc.stages.geometry_shader && desc.stages.geometry_shader->HasBindlessResources())
                            ||(desc.stages.mesh_shader && desc.stages.mesh_shader->HasBindlessResources())
                            ||(desc.stages.task_shader && desc.stages.task_shader->HasBindlessResources());

    BuildPipelineResourceIndex();

    // TryLocateAndStripBindlessTableUniformBuffer();
    depth_test_enable_ = desc.depth_stencil.depth_test_enable;
    vertex_inputs_    = desc.stages.vertex_shader->GetVertexInputDesc();
    fragment_outputs_ = desc.stages.fragment_shader->GetFragmentOutputDesc();
    if(desc.color_attachments.size() != fragment_outputs_.size()) {
        MI_LOG(MIInfraLogType::kWarning, "Color attachment count mismatch");
        return ;
    }
    for(int i = 0; i < desc.color_attachments.size(); ++i) {
        if(fragment_outputs_[i].format == RHIFragmentOutputFormatType::k4xFp32) {
            if(!IsFloatPixelFormat(desc.color_attachments[i].format)) {
                MI_LOG(MIInfraLogType::kWarning, "Color attachment {} ({}) format mismatch,"
                                                 "provided {}, reflected {}", i, fragment_outputs_[i].name,
                                                 GetPixelFormatName(desc.color_attachments[i].format),
                                                 GetRHIFragmentOutputFormatName(fragment_outputs_[i].format));
                return ;
            }
        } else if(fragment_outputs_[i].format == RHIFragmentOutputFormatType::k4xUIint32) {
            if(!IsUIntPixelFormat(desc.color_attachments[i].format)) {
                MI_LOG(MIInfraLogType::kWarning, "Color attachment {} ({}) format mismatch,"
                                                 "provided {}, reflected {}",
                                                 i, fragment_outputs_[i].name,
                                                 GetPixelFormatName(desc.color_attachments[i].format),
                                                 GetRHIFragmentOutputFormatName(fragment_outputs_[i].format));
                return ;
            }
        }
    }
    if(depth_test_enable_ && desc.depth_stencil_attachment.format != PixelFormatType::kD32_FLOAT) {
        MI_LOG(MIInfraLogType::kWarning, "Depth stencil attachment format mismatch");
        return ;
    }

    if(!CompileRHI(desc)) {
        MI_LOG(MIInfraLogType::kWarning, "Pipeline {} assemble failed.", GetName());
        return;
    }
    is_valid_ = true;
}

void RHIGraphicsPipeline::Reset() {
    depth_test_enable_ = false;
    vertex_inputs_.clear();
    fragment_outputs_.clear();
    pipeline_resource_index_.clear();
    RHIPipeline::Reset();
}

void RHIComputePipeline::Compile(mi::RHIShader *compute_shader) {
    Reset();
    if(!CheckAndRemapShaderResources(compute_shader)) return;
    if(!CheckNoOverlappingNamesAmongDifferentTypes()) return;
    BuildPipelineResourceIndex();
    has_bindless_resources_ = compute_shader->HasBindlessResources();
    if(!CompileRHI(compute_shader)) return;
    is_valid_ = true;
}

void RHIRayTracingPipeline::Reset() {
    RHIPipeline::Reset();

    shader_group_count_ = 0;
    max_recursion_depth_ = 1;
    raygen_group_count_ = 0;
    miss_group_count_ = 0;
    hit_group_count_ = 0;
    callable_group_count_ = 0;
}

void RHIRayTracingPipeline::Compile(const RHIRayTracingPipelineDesc& desc) {
    Reset();

    // Check all shaders in the pipeline
    for (auto* shader : desc.shaders) {
        if (!CheckAndRemapShaderResources(shader)) return;
    }

    if (!CheckNoOverlappingNamesAmongDifferentTypes()) return;

    // Build pipeline resource index
    BuildPipelineResourceIndex();

    // Check if any shader has bindless resources
    has_bindless_resources_ = false;
    for (auto* shader : desc.shaders) {
        if (shader && shader->HasBindlessResources()) {
            has_bindless_resources_ = true;
            break;
        }
    }

    // Store pipeline configuration
    shader_group_count_ = static_cast<uint32_t>(desc.shader_groups.size());
    max_recursion_depth_ = desc.max_recursion_depth;

    // Validate shader group configuration
    for (const auto& group : desc.shader_groups) {
        switch (group.type) {
            case RHIRayTracingShaderGroupType::kRayGeneration:
            case RHIRayTracingShaderGroupType::kMiss:
            case RHIRayTracingShaderGroupType::kCallable:
                if (group.general_shader_index >= desc.shaders.size()) {
                    MI_LOG(MIInfraLogType::kWarning, "Invalid general shader index in ray tracing pipeline");
                    return;
                }
                break;
            case RHIRayTracingShaderGroupType::kTrianglesHitGroup:
            case RHIRayTracingShaderGroupType::kProceduralHitGroup:
                if (group.closest_hit_shader_index != UINT32_MAX &&
                    group.closest_hit_shader_index >= desc.shaders.size()) {
                    MI_LOG(MIInfraLogType::kWarning, "Invalid closest hit shader index in ray tracing pipeline");
                    return;
                }
                if (group.any_hit_shader_index != UINT32_MAX &&
                    group.any_hit_shader_index >= desc.shaders.size()) {
                    MI_LOG(MIInfraLogType::kWarning, "Invalid any hit shader index in ray tracing pipeline");
                    return;
                }
                if (group.type == RHIRayTracingShaderGroupType::kProceduralHitGroup &&
                    group.intersection_shader_index != UINT32_MAX &&
                    group.intersection_shader_index >= desc.shaders.size()) {
                    MI_LOG(MIInfraLogType::kWarning, "Invalid intersection shader index in ray tracing pipeline");
                    return;
                }
                break;
            default:
                MI_LOG(MIInfraLogType::kWarning, "Invalid shader group type in ray tracing pipeline");
                return;
        }
    }

    // Calculate shader group counts by type
    for (const auto& group : desc.shader_groups) {
        switch (group.type) {
            case RHIRayTracingShaderGroupType::kRayGeneration:
                raygen_group_count_++;
                break;
            case RHIRayTracingShaderGroupType::kMiss:
                miss_group_count_++;
                break;
            case RHIRayTracingShaderGroupType::kTrianglesHitGroup:
            case RHIRayTracingShaderGroupType::kProceduralHitGroup:
                hit_group_count_++;
                break;
            case RHIRayTracingShaderGroupType::kCallable:
                callable_group_count_++;
                break;
            default: assert(false && "Unknown ray tracing shader group type");
        }
    }

    // Compile the RHI-specific implementation
    if (!CompileRHI(desc)) {
        MI_LOG(MIInfraLogType::kWarning, "Ray tracing pipeline {} assemble failed.", GetName());
        return;
    }

    is_valid_ = true;
}

MI_NAMESPACE_END