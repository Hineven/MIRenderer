/*
 * Created: 2025/3/21
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_param.h>
#include <rdg/rdg_pass.h>
#include <rdg/rdg_pass_param_table.h>
#include <rdg/rdg_resource.h>
#include <rdg/rdg_shader.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_as.h>

MI_NAMESPACE_BEGIN

// A simple implementation of parameter table id allocation.
// Only the renderer thread will allocate parameter table ids, so no synchronization is needed.
// Recycling is not necessary as rhi will manage the lifecycle of parameter tables.
static uint32_t tl_next_table_id = 0;

// Derive the narrowest shader stage mask for a push constant by inspecting each stage shader's
// reflection. Only stages whose reflection reports a push constant (HasPushConstant) are added,
// so vkCmdPushConstants writes only to stages that actually read the data. The mask is always a
// subset of the pipeline layout's push constant range (declared with eAll), so any subset is valid.
RHIShaderFrequencyFlags RDGCommandHelper::PushConstantStagesFor(const RDGShader * shader) {
    if (!shader) return RHIShaderFrequencyFlagBits::kAll;
    const auto & s = shader->shaders_;
    RHIShaderFrequencyFlags flags = RHIShaderFrequencyFlagBits::kNone;
    switch (shader->GetPipelineType()) {
        case RHIPipelineType::kCompute:
            // Single stage; if the compute shader has no push constant the dispatch helper won't
            // call PushConstants at all, so kCompute is always correct here.
            return RHIShaderFrequencyFlagBits::kCompute;
        case RHIPipelineType::kGraphics:
            if (s.vertex   && s.vertex->HasPushConstant())   flags |= RHIShaderFrequencyFlagBits::kVertex;
            if (s.fragment && s.fragment->HasPushConstant()) flags |= RHIShaderFrequencyFlagBits::kFragment;
            if (s.geometry && s.geometry->HasPushConstant()) flags |= RHIShaderFrequencyFlagBits::kGeometry;
            if (s.task     && s.task->HasPushConstant())     flags |= RHIShaderFrequencyFlagBits::kTask;
            if (s.mesh     && s.mesh->HasPushConstant())     flags |= RHIShaderFrequencyFlagBits::kMesh;
            break;
        case RHIPipelineType::kRayTracing:
            if (s.raygen   && s.raygen->HasPushConstant())   flags |= RHIShaderFrequencyFlagBits::kRaygen;
            if (s.miss     && s.miss->HasPushConstant())     flags |= RHIShaderFrequencyFlagBits::kMiss;
            if (s.callable && s.callable->HasPushConstant()) flags |= RHIShaderFrequencyFlagBits::kCallable;
            for (const auto & ch : s.closest_hit)
                if (ch && ch->HasPushConstant()) { flags |= RHIShaderFrequencyFlagBits::kClosestHit; break; }
            for (const auto & ah : s.any_hit)
                if (ah && ah->HasPushConstant()) { flags |= RHIShaderFrequencyFlagBits::kAnyHit; break; }
            break;
    }
    // Safe fallback: if no stage reported a push constant (e.g. shaders not yet compiled, or the
    // shader genuinely has none), fall back to kAll rather than emitting an invalid empty stage mask.
    return flags ? flags : RHIShaderFrequencyFlagBits::kAll;
}

std::optional<RHIBindPipelineParametersDesc> RDGCommandHelper::BuildParameterDesc(
    RHICommandQueueGraphics & queue,
    const RDGPassParameterTable & table,
    RenderGraph * graph) {
    auto * info = table.info_;
    auto * params = table.params_ptr_;
    if (!info || !params || !table.root_signature_) return std::nullopt;

    RHIBindPipelineParametersDesc ret = {};
    int num_ref_uniform_buffers = (int)info->uniform_buffers_.size();
    int num_uniform_buffers = 0;
    if (num_ref_uniform_buffers) {
        ret.uniforms = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(num_ref_uniform_buffers), num_ref_uniform_buffers);
        for (int i = 0; i < num_ref_uniform_buffers; i++) {
            auto struct_ptr = *(void**)((uint8_t*)params + info->uniform_buffers_[i].cpp_offset);
            if (RDGParameter_IsUnsetPointer(struct_ptr)) {
                MI_WARN("RootSignature {}: Referenced uniform buffer pointer {} is unset.",
                    table.root_signature_->GetName(), info->uniform_buffers_[i].info->name);
                return std::nullopt;
            }
            if (struct_ptr) {
                auto buffer_ptr = graph->GetUniformBufferForParameterStruct(struct_ptr);
                if (buffer_ptr.buffer) {
                    auto span = buffer_ptr.buffer->GetRHI();
                    span.offset += buffer_ptr.offset;
                    ret.uniforms[num_uniform_buffers ++] = {span, (uint32_t)i};
                } else {
                    // Bind a null descriptor.
                    ret.uniforms[num_uniform_buffers ++] = {{}, (uint32_t)i};
                }
            } else {
                // Bind a null descriptor.
                ret.uniforms[num_uniform_buffers ++] = {{}, (uint32_t)i};
            }
        }
        ret.uniforms = ret.uniforms.first(num_uniform_buffers);
    }
    {
        int num_active_storages = 0;
        ret.storages = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(info->storage_buffers_.size()), info->storage_buffers_.size());
        for (const auto& [i, e] : std::views::enumerate(info->storage_buffers_)) {
            auto buffer_ptr = *static_cast<RDGBuffer**>((void*)((uint8_t*)params + e.cpp_offset));
            if (RDGParameter_IsUnsetPointer(buffer_ptr)) {
                MI_WARN("RootSignature {}: Referenced storage buffer pointer {} is unset.",
                    table.root_signature_->GetName(), e.info->name);
                return std::nullopt;
            }
            ret.storages[num_active_storages ++] = {buffer_ptr ? buffer_ptr->GetRHI() : RHIBufferSpan{}, (uint32_t)i};
        }
        ret.storages = ret.storages.first(num_active_storages);
    }
    {
        int num_active_uavs = 0;
        ret.uavs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(info->uavs_.size()), info->uavs_.size());
        for (const auto& [i, e] : std::views::enumerate(info->uavs_)) {
            auto texture_desc = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params + e.cpp_offset));
            if (RDGParameter_IsUnsetPointer(texture_desc.texture)) {
                MI_WARN("RootSignature {}: Referenced UAV texture pointer {} is unset.",
                    table.root_signature_->GetName(), e.info->name);
                return std::nullopt;
            }
            auto layout = table.GetTextureLayout(RHIPipelineResourceType::kUAV, (uint32_t)i);
            ret.uavs[num_active_uavs ++] = {texture_desc.texture ? texture_desc.texture->GetRHI() : nullptr, (uint32_t)i, texture_desc.array_layer, texture_desc.mip_level, layout};
        }
        ret.uavs = ret.uavs.first(num_active_uavs);
    }
    {
        int num_active_srvs = 0;
        ret.srvs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(info->srvs_.size()), info->srvs_.size());
        for (const auto& [i, e] : std::views::enumerate(info->srvs_)) {
            auto texture_desc = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params + e.cpp_offset));
            if (RDGParameter_IsUnsetPointer(texture_desc.texture)) {
                MI_WARN("RootSignature {}: Referenced SRV texture pointer {} is unset.",
                    table.root_signature_->GetName(), e.info->name);
                return std::nullopt;
            }
            auto layout = table.GetTextureLayout(RHIPipelineResourceType::kSRV, (uint32_t)i);
            ret.srvs[num_active_srvs ++] = {texture_desc.texture ? texture_desc.texture->GetRHI() : nullptr, (uint32_t)i, texture_desc.array_layer, texture_desc.mip_level, layout};
        }
        ret.srvs = ret.srvs.first(num_active_srvs);
    }
    {
        int num_active_samplers = 0;
        ret.samplers = std::span(queue.Allocate<RHIPipelineParameterResourceDesc[]>(info->samplers_.size()), info->samplers_.size());
        for (const auto& [i, e] : std::views::enumerate(info->samplers_)) {
            auto sampler_ptr = *static_cast<RHISampler**>((void*)((uint8_t*)params + e.cpp_offset));
            if (!sampler_ptr || RDGParameter_IsUnsetPointer(sampler_ptr)) {
                MI_WARN("RootSignature {}: Referenced sampler pointer {} is null/unset.",
                    table.root_signature_->GetName(), e.info->name);
                return std::nullopt;
            }
            ret.samplers[num_active_samplers ++] = {sampler_ptr, (uint32_t)i};
        }
        ret.samplers = ret.samplers.first(num_active_samplers);
    }
    {
        int num_active_acceleration_structures = 0;
        ret.acceleration_structures = std::span(queue.Allocate<RHIPipelineParameterResourceDesc[]>(info->acceleration_structures_.size()), info->acceleration_structures_.size());
        for (const auto& [i, e] : std::views::enumerate(info->acceleration_structures_)) {
            auto as_ptr = *static_cast<RHIAccelerationStructure**>((void*)((uint8_t*)params + e.cpp_offset));
            if (RDGParameter_IsUnsetPointer(as_ptr)) {
                MI_WARN("RootSignature {}: Referenced AS pointer {} is unset.",
                    table.root_signature_->GetName(), e.info->name);
                return std::nullopt;
            }
            ret.acceleration_structures[num_active_acceleration_structures ++] = {as_ptr, (uint32_t)i};
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

uint32_t RDGCommandHelper::AllocateParameterTableIds(uint32_t count) {
    uint32_t base = tl_next_table_id;
    tl_next_table_id += count;
    return base;
}

void RDGCommandHelper::Dispatch(RHICommandQueueGraphics & queue, RDGShader * shader,
    uint32_t table_id, uint32_t x, uint32_t y, uint32_t z,
    std::span<std::byte> push_constants) {
    if (!shader || !shader->IsValid()) {
        MI_WARN("Invalid compute shader. Dispatch cancelled.");
        return;
    }
    queue.BindPipeline(shader->compute_pipeline_.Raw(), shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kCompute);
    if (!push_constants.empty()) queue.PushConstants(push_constants, RHIBindPointType::kCompute, PushConstantStagesFor(shader));
    queue.Dispatch(x, y, z);
}

void RDGCommandHelper::DispatchIndirect(RHICommandQueueGraphics & queue, RDGShader * compute_shader,
    uint32_t table_id, RDGBuffer * indirect_buffer, uint32_t offset,
    std::span<std::byte> push_constants) {
    if (!compute_shader || !compute_shader->IsValid()) {
        MI_WARN("Invalid compute shader. DispatchIndirect cancelled.");
        return;
    }
    queue.BindPipeline(compute_shader->compute_pipeline_.Raw(), compute_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kCompute);
    if (!push_constants.empty()) queue.PushConstants(push_constants, RHIBindPointType::kCompute, PushConstantStagesFor(compute_shader));
    queue.DispatchIndirect(indirect_buffer->GetRHI().buffer, uint32_t(indirect_buffer->GetRHI().offset + offset));
}

void RDGCommandHelper::BeginGraphicsRender(RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
    uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
    const void * params, std::span<std::byte> push_constants) {
    if (!graphics_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid shader. BeginGraphicsRender cancelled.", graphics_shader->class_registry_->name);
        return;
    }
    queue.BindPipeline(graphics_shader->graphics_pipeline_.Raw(), graphics_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kGraphics);
    if (!push_constants.empty()) queue.PushConstants(push_constants, RHIBindPointType::kGraphics, PushConstantStagesFor(graphics_shader));

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
    const void * params, int vertex_count, int instance_count, int first_vertex, int first_instance,
    std::span<std::byte> push_constants) {
    BeginGraphicsRender(queue, graphics_shader, table_id, render_pass_info, params, push_constants);
    queue.Draw(vertex_count, instance_count, first_vertex, first_instance);
    EndGraphicsRender(queue);
}

void RDGCommandHelper::DispatchRays(RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
    uint32_t table_id, uint32_t width, uint32_t height, uint32_t depth,
    std::span<std::byte> push_constants) {
    if (!ray_tracing_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid raytracing shader. Dispatch cancelled.",
            ray_tracing_shader->class_registry_->name);
        return;
    }
    queue.BindPipeline(ray_tracing_shader->ray_tracing_pipeline_.Raw(), ray_tracing_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kRayTracing);
    if (!push_constants.empty()) queue.PushConstants(push_constants, RHIBindPointType::kRayTracing, PushConstantStagesFor(ray_tracing_shader));
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
    uint32_t table_id, RDGBuffer * indirect_buffer, std::span<std::byte> push_constants) {
    if (!ray_tracing_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid raytracing shader. DispatchRaysIndirect cancelled.",
            ray_tracing_shader->class_registry_->name);
        return;
    }
    queue.BindPipeline(ray_tracing_shader->ray_tracing_pipeline_.Raw(), ray_tracing_shader->root_signature_->GetRootSignature());
    queue.BindSignatureParameterTable(table_id, RHIBindPointType::kRayTracing);
    if (!push_constants.empty()) queue.PushConstants(push_constants, RHIBindPointType::kRayTracing, PushConstantStagesFor(ray_tracing_shader));
    auto sbt = ray_tracing_shader->GetSBTBuffers(queue);
    queue.DispatchRaysIndirect(sbt.raygen, sbt.miss, sbt.miss_stride, sbt.hit, sbt.hit_stride, indirect_buffer->GetRHI());
}

MI_NAMESPACE_END
