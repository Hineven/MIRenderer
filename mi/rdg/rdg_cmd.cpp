/*
 * Created: 2025/3/21
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_param.h>
#include <rdg/rdg_pass.h>
#include <rdg/rdg_resource.h>
#include <rdg/rdg_shader.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_as.h>

MI_NAMESPACE_BEGIN

// A simple implementation of parameter table id allocation.
// Only the renderer thread will call CreateParameterTable, so no synchronization is needed. 
// Recycling is not necessary as rhi will manage the lifecycle of parameter tables. 
static uint32_t tl_next_table_id = 0;

std::optional<RHIBindPipelineParametersDesc> BuildParameterDesc(
    RDGPass * pass, RDGShader * shader, RHICommandQueueGraphics & queue,
    const RDGShaderSignatureParamInfo * info, const void * params,
    bool populate_all) {
    RHIBindPipelineParametersDesc ret = {};
    int num_ref_uniform_buffers = (int)info->uniform_buffers_.size();
    int num_uniform_buffers = 0;
    if (num_ref_uniform_buffers) {
        ret.uniforms = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(num_ref_uniform_buffers), num_ref_uniform_buffers);
        for (int i = 0; i < num_ref_uniform_buffers; i++) {
            auto struct_ptr = *(void**)((uint8_t*)params + info->uniform_buffers_[i].cpp_offset);
            uint32_t slot = populate_all ? (uint32_t)i
                : shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUniformBuffer>((int)i);
            if (slot != UINT32_MAX) {
                if (!struct_ptr || RDGParameter_IsUnsetPointer(struct_ptr)) {
                    if (!populate_all) {
                        MI_WARN("Shader {}: Referenced uniform buffer pointer {} is null/unset.",
                            shader->GetShaderClassRegistry()->name,
                            info->uniform_buffers_[i].info->name);
                        return std::nullopt;
                    }
                    continue;
                }
                auto buffer_ptr = pass->GetGraph()->GetUniformBufferForParameterStruct(struct_ptr);
                if (!buffer_ptr.buffer) {
#ifndef NDEBUG
                    if (!populate_all && pass->shader_ && pass->shader_->QueryShaderAccess(info->uniform_buffers_[i].info->name).access
                        != RHIGPUAccessFlagBits::kNone) {
                            MI_WARN("Shader {}: Can not find pre-allocated uniform buffer {}.",
                                shader->GetShaderClassRegistry()->name,
                                info->uniform_buffers_[i].info->name);
                            return std::nullopt;
                    }
#endif
                } else {
                    auto span = buffer_ptr.buffer->GetRHI();
                    span.offset += buffer_ptr.offset;
                    ret.uniforms[num_uniform_buffers ++] = {span, slot};
                }
            }
        }
        ret.uniforms = ret.uniforms.first(num_uniform_buffers);
    }
    {
        int num_active_storages = 0;
        ret.storages = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(info->storage_buffers_.size()), info->storage_buffers_.size());
        for (const auto& [i, e] : std::views::enumerate(info->storage_buffers_)) {
            auto buffer_ptr = *static_cast<RDGBuffer**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = populate_all ? (uint32_t)i
                : shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kStorageBuffer>((int)i);
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(buffer_ptr)) {
                    if (!populate_all) {
                        MI_WARN("Shader {}: Referenced storage buffer pointer {} is unset.",
                            shader->GetName(), e.info->name);
                        return std::nullopt;
                    }
                    continue;
                }
                if (buffer_ptr) {
                    ret.storages[num_active_storages ++] = {buffer_ptr->GetRHI(), slot};
                }
            }
        }
        ret.storages = ret.storages.first(num_active_storages);
    }
    {
        int num_active_uavs = 0;
        ret.uavs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(info->uavs_.size()), info->uavs_.size());
        for (const auto& [i, e] : std::views::enumerate(info->uavs_)) {
            auto texture_desc = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = populate_all ? (uint32_t)i
                : shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUAVTexture>((int)i);
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(texture_desc.texture)) {
                    if (!populate_all) {
                        MI_WARN("Shader {}: Referenced UAV texture pointer {} is unset.",
                            shader->GetName(), e.info->name);
                        return std::nullopt;
                    }
                    continue;
                }
                if (texture_desc.texture) {
                    ret.uavs[num_active_uavs ++] = {texture_desc.texture->GetRHI(), slot, texture_desc.array_layer, texture_desc.mip_level};
                }
            }
        }
        ret.uavs = ret.uavs.first(num_active_uavs);
    }
    {
        int num_active_srvs = 0;
        ret.srvs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(info->srvs_.size()), info->srvs_.size());
        for (const auto& [i, e] : std::views::enumerate(info->srvs_)) {
            auto texture_desc = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = populate_all ? (uint32_t)i
                : shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kSRVTexture>((int)i);
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(texture_desc.texture)) {
                    if (!populate_all) {
                        MI_WARN("Shader {}: Referenced SRV texture pointer {} is unset.",
                            shader->GetName(), e.info->name);
                        return std::nullopt;
                    }
                    continue;
                }
                if (texture_desc.texture) {
                    ret.srvs[num_active_srvs ++] = {texture_desc.texture->GetRHI(), slot, texture_desc.array_layer, texture_desc.mip_level};
                }
            }
        }
        ret.srvs = ret.srvs.first(num_active_srvs);
    }
    {
        int num_active_samplers = 0;
        ret.samplers = std::span(queue.Allocate<RHIPipelineParameterResourceDesc[]>(info->samplers_.size()), info->samplers_.size());
        for (const auto& [i, e] : std::views::enumerate(info->samplers_)) {
            auto sampler_ptr = *static_cast<RHISampler**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = populate_all ? (uint32_t)i
                : shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kSampler>((int)i);
            if (slot != UINT32_MAX) {
                if (!sampler_ptr || RDGParameter_IsUnsetPointer(sampler_ptr)) {
                    if (!populate_all) {
                        MI_WARN("Shader {}: Referenced sampler pointer {} is null/unset.", shader->GetName(), e.info->name);
                        return std::nullopt;
                    }
                    continue;
                }
                ret.samplers[num_active_samplers ++] = {sampler_ptr, slot};
            }
        }
        ret.samplers = ret.samplers.first(num_active_samplers);
    }
    {
        int num_active_acceleration_structures = 0;
        ret.acceleration_structures = std::span(queue.Allocate<RHIPipelineParameterResourceDesc[]>(info->acceleration_structures_.size()), info->acceleration_structures_.size());
        for (const auto& [i, e] : std::views::enumerate(info->acceleration_structures_)) {
            auto as_ptr = *static_cast<RHIAccelerationStructure**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = populate_all ? (uint32_t)i
                : shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kAccelerationStructure>((int)i);
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(as_ptr)) {
                    if (!populate_all) {
                        MI_WARN("Shader {}: Referenced AS pointer {} is unset.",
                            shader->GetName(), e.info->name);
                        return std::nullopt;
                    }
                    continue;
                }
                ret.acceleration_structures[num_active_acceleration_structures ++] = {as_ptr, slot};
            }
        }
        ret.acceleration_structures = ret.acceleration_structures.first(num_active_acceleration_structures);
    }
    return ret;
}

uint32_t RDGCommandHelper::AllocateParameterTableId() {
    uint32_t ret = tl_next_table_id;
    tl_next_table_id ++;
    return ret;
}

uint32_t RDGCommandHelper::CreateParameterTable(
    RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * shader,
    const RDGShaderSignatureParamInfo * info, const void * params,
    bool populate_all) {
    uint32_t table_id = tl_next_table_id;
    auto desc = BuildParameterDesc(pass, shader, queue, info, params, populate_all);
    if (!desc.has_value()) {
        return UINT32_MAX;
    }
    queue.CreateSignatureParameterTable(table_id, shader->root_signature_->GetRootSignature(), desc.value());
    tl_next_table_id ++;
    return table_id;
}

void RDGCommandHelper::Dispatch(RHICommandQueueGraphics & queue, RDGShader * shader,
    uint32_t table_id, uint32_t x, uint32_t y, uint32_t z) {
    if (!shader || !shader->IsValid()) {
        MI_WARN("Invalid compute shader. Dispatch cancelled.");
        return;
    }
    queue.BindPipeline(shader->compute_pipeline_.Raw(), shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kCompute);
    queue.Dispatch(x, y, z);
}

void RDGCommandHelper::DispatchIndirect(RHICommandQueueGraphics & queue, RDGShader * compute_shader,
    uint32_t table_id, RDGBuffer * indirect_buffer, uint32_t offset) {
    if (!compute_shader || !compute_shader->IsValid()) {
        MI_WARN("Invalid compute shader. DispatchIndirect cancelled.");
        return;
    }
    queue.BindPipeline(compute_shader->compute_pipeline_.Raw(), compute_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kCompute);
    queue.DispatchIndirect(indirect_buffer->GetRHI().buffer, uint32_t(indirect_buffer->GetRHI().offset + offset));
}

void RDGCommandHelper::BeginGraphicsRender(RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
    uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
    const void * params) {
    if (!graphics_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid shader. BeginGraphicsRender cancelled.", graphics_shader->class_registry_->name);
        return;
    }
    queue.BindPipeline(graphics_shader->graphics_pipeline_.Raw(), graphics_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kGraphics);

    if (render_pass_info && !render_pass_info->vertex_buffers_.empty()) {
        for (auto e : render_pass_info->vertex_buffers_) {
            auto ptr = (RDGBuffer*)*(void**)((uint8_t*)params + e.cpp_offset);
            if (RDGParameter_IsUnsetPointer(ptr)) {
                MI_LOG(MIInfraLogType::kWarning, "Shader {}: Unset vertex buffer for parameter '{}'.",
                    graphics_shader->class_registry_->name, e.info->name);
            } else {
                if (ptr) queue.BindVertexBuffer(e.info->cpp_extra.vertex_buffer_info->index, ptr->GetRHI());
            }
        }
    }

    RHIDrawStateDesc ds {};
    if (render_pass_info && !graphics_shader->pipeline_config_.rasterization_discard && !render_pass_info->render_targets_.empty()) {
        auto reflected_frag_outputs = graphics_shader->graphics_pipeline_->GetFragmentOutputDesc();
        for (const auto& [i, e] : std::views::enumerate(render_pass_info->render_targets_)) {
            auto param = *(RDGShaderRenderTargetParameter*)((uint8_t*)params + e.cpp_offset);
            if (param.texture == nullptr) {
                MI_WARN("Shader {}: Null render target for parameter '{}'.",
                    graphics_shader->class_registry_->name, e.info->name);
                continue ;
            }
            auto target_index = e.info->cpp_extra.render_targets_info->target_index;
            auto to_bound = param.texture->GetRHI();
            if (target_index != UINT32_MAX) {
                if (reflected_frag_outputs.size() <= target_index) continue ;
                mi_warning(param.texture->GetDesc().usage & RHITextureUsageFlagBits::kRenderTarget,
                    "Shader {}: Assigned render target texture for {} is not created with kRenderTarget usage.",
                    graphics_shader->class_registry_->name, e.info->name);
                ds.SetAttachment(target_index, to_bound, param.load_op, param.store_op, param.clear_value,
                    param.array_layer == UINT64_MAX ? 0 : param.array_layer);
            } else {
                mi_warning(param.texture->GetDesc().usage & RHITextureUsageFlagBits::kDepthStencil,
                    "Shader {}: Assigned depth stencil target texture for {} is not created with kDepthStencil usage.",
                    graphics_shader->class_registry_->name, e.info->name);
                ds.SetDepthStencilAttachment(to_bound, param.load_op, param.store_op, param.clear_value);
            }
        }
    }

    queue.UpdateDrawState(ds);
    queue.BeginRendering();
}

void RDGCommandHelper::EndGraphicsRender(RHICommandQueueGraphics & queue) {
    queue.EndRendering();
}

void RDGCommandHelper::Draw(RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
    uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
    const void * params, int vertex_count, int instance_count, int first_vertex, int first_instance) {
    BeginGraphicsRender(queue, graphics_shader, table_id, render_pass_info, params);
    queue.Draw(vertex_count, instance_count, first_vertex, first_instance);
    EndGraphicsRender(queue);
}

void RDGCommandHelper::DispatchRays(RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
    uint32_t table_id, uint32_t width, uint32_t height, uint32_t depth) {
    if (!ray_tracing_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid raytracing shader. Dispatch cancelled.",
            ray_tracing_shader->class_registry_->name);
        return;
    }
    queue.BindPipeline(ray_tracing_shader->ray_tracing_pipeline_.Raw(), ray_tracing_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kRayTracing);
    auto sbt = ray_tracing_shader->GetSBTBuffers(queue);
    if (sbt.raygen && sbt.miss && sbt.hit) {
        queue.BindShaderBindingTable(sbt.raygen, sbt.miss, sbt.hit);
    } else {
        MI_WARN("Shader {}: SBT not set up correctly. Dispatch rays cancelled.",
            ray_tracing_shader->class_registry_->name);
        return;
    }
    queue.DispatchRays(width, height, depth);
}

void RDGCommandHelper::DispatchRaysIndirect(RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
    uint32_t table_id, RDGBuffer * indirect_buffer) {
    if (!ray_tracing_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid raytracing shader. DispatchRaysIndirect cancelled.",
            ray_tracing_shader->class_registry_->name);
        return;
    }
    queue.BindPipeline(ray_tracing_shader->ray_tracing_pipeline_.Raw(), ray_tracing_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kRayTracing);
    auto sbt = ray_tracing_shader->GetSBTBuffers(queue);
    queue.DispatchRaysIndirect(sbt.raygen, sbt.miss, sbt.miss_stride, sbt.hit, sbt.hit_stride, indirect_buffer->GetRHI());
}

MI_NAMESPACE_END
