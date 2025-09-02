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
std::optional<RHIBindPipelineParametersDesc> RDGCommandHelper::SetupShaderParams(
    RDGPass * pass, RDGShader * shader, RHICommandQueueGraphics & queue,
    const RDGShaderParamStructAndSizeInfo * base_info, const void * params) {
    RHIBindPipelineParametersDesc ret = {};
    // Bind uniform buffers
    int num_ref_uniform_buffers = (int)base_info->uniform_buffers_.size();
    int num_uniform_buffers = 0;
    if (num_ref_uniform_buffers) {
        ret.uniforms = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(num_ref_uniform_buffers), num_ref_uniform_buffers);
        for (int i = 0; i < num_ref_uniform_buffers; i++) {
            auto struct_ptr = *(void**)((uint8_t*)params + base_info->uniform_buffers_[i].cpp_offset);
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUniformBuffer>((int)i);
            if (slot != UINT32_MAX) {
                if (!struct_ptr) {
                    MI_WARN("Shader {}: Referenced uniform buffer pointer {} is null, which should not happen.",
                        shader->GetShaderClassRegistry()->name,
                        base_info->uniform_buffers_[i].info->name);
                    return std::nullopt;
                } else {
                    if (RDGParameter_IsUnsetPointer(struct_ptr)) {
                        MI_WARN("Shader {}: Referenced uniform buffer pointer {} is unset, which should not happen.",
                            shader->GetShaderClassRegistry()->name,
                            base_info->uniform_buffers_[i].info->name);
                        return std::nullopt;
                    }
                    auto buffer_ptr = pass->GetGraph()->GetUniformBufferForParameterStruct(struct_ptr);
                    if (!buffer_ptr.buffer) {
                        MI_WARN("Shader {}: Can not find pre-allocated uniform buffer {} from the render graph. Draw cancelled.",
                            shader->GetShaderClassRegistry()->name,
                            base_info->uniform_buffers_[i].info->name);
                        return std::nullopt;
                    }
                    auto span = buffer_ptr.buffer->GetRHI();
                    span.offset += buffer_ptr.offset;
                    ret.uniforms[num_uniform_buffers ++] = {span, slot};
                }
            } // Otherwise, silently ignore the case that the shader is not using this uniform buffer at all.
        }
        ret.uniforms = ret.uniforms.first(num_uniform_buffers);
    }
    // Bind storage buffers
    {
        int num_active_storages = 0;
        ret.storages = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(base_info->storage_buffers_.size()), base_info->storage_buffers_.size());
        for (const auto& [i, e] : std::views::enumerate(base_info->storage_buffers_)) {
            auto buffer_ptr = *static_cast<RDGBuffer**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kStorageBuffer>((int)i);
            // Ignore those parameters set in params but not used in the shader.
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(buffer_ptr)) {
                    MI_WARN("Shader {}: Referenced storage buffer pointer {} is unset, which should not happen.",
                        shader->GetName(),
                        e.info->name
                    );
                    return std::nullopt;
                }
                ret.storages[num_active_storages ++] = {buffer_ptr ? buffer_ptr->GetRHI() : RHIBufferSpan{}, slot};
            } // Otherwise potentially the shader is not using this storage buffer. Silently ignore it.
        }
        ret.storages = ret.storages.first(num_active_storages);
    }
    // Bind UAVs
    {
        int num_active_uavs = 0;
        ret.uavs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(base_info->uavs_.size()), base_info->uavs_.size());
        for (const auto& [i, e] : std::views::enumerate(base_info->uavs_)) {
            auto texture_desc = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params + e.cpp_offset));
            auto texture_ptr = texture_desc.texture;
            auto base_array_layer = texture_desc.array_layer;
            auto mip_level = texture_desc.mip_level;
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUAVTexture>((int)i);
            // Ignore those parameters set in params but not used in the shader.
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(texture_ptr)) {
                    MI_WARN("Shader {}: "
                            "Referenced UAV texture pointer {} is unset, which should not happen.",
                            shader->GetName(), e.info->name);
                    return std::nullopt;
                }
                ret.uavs[num_active_uavs ++] = {texture_ptr ? texture_ptr->GetRHI() : nullptr, slot, base_array_layer, mip_level};
            } // Otherwise potentially the shader is not using this UAV. Silently ignore it.
        }
        ret.uavs = ret.uavs.first(num_active_uavs);
    }
    // Bind SRVs
    {
        int num_active_srvs = 0;
        ret.srvs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(base_info->srvs_.size()), base_info->srvs_.size());
        for (const auto& [i, e] : std::views::enumerate(base_info->srvs_)) {
            auto texture_desc = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params + e.cpp_offset));
            auto texture_ptr = texture_desc.texture;
            auto base_array_layer = texture_desc.array_layer;
            // Though this is not used for SRVs. We still copy the value for consistency.
            uint32_t mip_level = texture_desc.mip_level;
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kSRVTexture>((int)i);
            // Ignore those parameters set in params but not used in the shader.
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(texture_ptr)) {
                    MI_WARN("Shader {}: Referenced SRV texture pointer {} is unset, which should not happen.",
                        shader->GetName(), e.info->name);
                    return std::nullopt;
                }
                ret.srvs[num_active_srvs ++] = {texture_ptr ? texture_ptr->GetRHI() : nullptr, slot, base_array_layer, mip_level};
            } // Otherwise potentially the shader is not using this SRV. Silently ignore it.
        }
        ret.srvs = ret.srvs.first(num_active_srvs);
    }
    // Bind samplers
    {
        int num_active_samplers = 0;
        ret.samplers = std::span(queue.Allocate<RHIPipelineParameterResourceDesc[]>(base_info->samplers_.size()), base_info->samplers_.size());
        for (const auto& [i, e] : std::views::enumerate(base_info->samplers_)) {
            auto sampler_ptr = *static_cast<RHISampler**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kSampler>((int)i);
            // Ignore those parameters set in params but not used in the shader.
            if (slot != UINT32_MAX) {
                if (!sampler_ptr || RDGParameter_IsUnsetPointer(sampler_ptr)) {
                    MI_WARN("Shader {}: Referenced sampler pointer {} is null/unset, which should not happen.", shader->GetName(), e.info->name);
                    return std::nullopt;
                }
                ret.samplers[num_active_samplers ++] = {sampler_ptr, slot};
            } // Otherwise potentially the shader is not using this sampler. Silently ignore it.
        }
        ret.samplers = ret.samplers.first(num_active_samplers);
    }
    {
        int num_active_acceleration_structures = 0;
        ret.acceleration_structures = std::span(queue.Allocate<RHIPipelineParameterResourceDesc[]>(base_info->acceleration_structures_.size()), base_info->acceleration_structures_.size());
        for (const auto& [i, e] : std::views::enumerate(base_info->acceleration_structures_)) {
            auto as_ptr = *static_cast<RHIAccelerationStructure**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kAccelerationStructure>((int)i);
            // Ignore those parameters set in params but not used in the shader.
            if (slot != UINT32_MAX) {
                if (RDGParameter_IsUnsetPointer(as_ptr)) {
                    MI_WARN("Shader {}: Referenced AS pointer {} is unset, which should not happen.",
                        shader->GetName(), e.info->name);
                    return std::nullopt;
                }
                ret.acceleration_structures[num_active_acceleration_structures ++] = {as_ptr, slot};
            } // Otherwise potentially the shader is not using this AS. Silently ignore it.
        }
        ret.acceleration_structures = ret.acceleration_structures.first(num_active_acceleration_structures);
    }
    return ret;
}

bool RDGCommandHelper::BindGraphicsShader (
    RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params,
    bool manual_vbuffer) {
    if (!graphics_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid shader. Draw cancelled.",
            graphics_shader->class_registry_->name);
        return false;
    }
    auto desc = SetupShaderParams(pass, graphics_shader, queue, info, params);
    if (!desc.has_value()) {
        MI_WARN("Shader {}: Failed to upload shader parameters. Draw cancelled.",
            graphics_shader->class_registry_->name);
        return false;
    }
    queue.BindPipeline(graphics_shader->graphics_pipeline_.Raw());
    if (!info->vertex_buffers_.empty()) {
        for (auto e : info->vertex_buffers_) {
            auto ptr = (RDGBuffer*)*(void**)((uint8_t*)params + e.cpp_offset);
            if (ptr) {
                if (!manual_vbuffer) {
                    queue.BindVertexBuffer(
                        e.info->cpp_extra.vertex_buffer_info->index,
                        ((RDGBuffer*)*(void**)((uint8_t*)params + e.cpp_offset))->GetRHI()
                    );
                }
            } else {
                if (!manual_vbuffer) {
                    MI_LOG(MIInfraLogType::kWarning, "Shader {}: Null vertex buffer for paramter '{}'. Bind cancelled.",
                        graphics_shader->class_registry_->name, e.info->name);
                    // cancel the draw
                    return false;
                }
            }
        }
    }
    RHIDrawDesc ds {};
    if (!info->render_targets_.empty()) {
        for (const auto& [i, e] : std::views::enumerate(info->render_targets_)) {
            auto param = *(RDGShaderRenderTargetParameter*)((uint8_t*)params + e.cpp_offset);
            RHITexture * to_bound = nullptr;
            if (param.texture == nullptr) {
                MI_WARN("Shader {}: Null render target for paramter '{}'. It will not be drawn.",
                    graphics_shader->class_registry_->name, e.info->name);
                continue ;
            }
            to_bound = param.texture->GetRHI();
            if (e.info->cpp_extra.render_targets_info->target_index != UINT32_MAX) {
                mi_warning(param.texture->GetDesc().usage & RHITextureUsageFlagBits::kRenderTarget,
                    "Shader {}: Assigned render target texture for {} is not created with kRenderTarget usage.",
                    graphics_shader->class_registry_->name, e.info->name);
                ds.SetAttachment(
                    e.info->cpp_extra.render_targets_info->target_index, to_bound,
                    param.load_op, param.store_op, param.clear_value,
                    param.array_layer == UINT64_MAX ? 0 : param.array_layer
                );
            } else {
                mi_warning(param.texture->GetDesc().usage & RHITextureUsageFlagBits::kDepthStencil,
                    "Shader {}: Assigned depth stencil target texture for {} is not created with kDepthStencil usage.",
                    graphics_shader->class_registry_->name, e.info->name);
                ds.SetDepthStencilAttachment(to_bound, param.load_op, param.store_op, param.clear_value);
            }
        }
    }
    queue.BindPipelineParameters(RHIBindPointType::kGraphics, desc.value());
    queue.UpdateDrawState(ds);
    return true;
}

bool RDGCommandHelper::BindComputeShader (
    RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params) {
    if (!compute_shader || !compute_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid shader. Dispatch cancelled.",
            compute_shader->class_registry_->name);
        return false;
    }
    auto desc = SetupShaderParams(pass, compute_shader, queue, info, params);
    if (!desc.has_value()) {
        MI_WARN("Shader {}: Failed to upload shader parameters. Dispatch cancelled.",
            compute_shader->class_registry_->name);
        return false;
    }

    queue.BindPipeline(compute_shader->compute_pipeline_.Raw());
    queue.BindPipelineParameters(RHIBindPointType::kCompute, desc.value());

    return true;
}

bool RDGCommandHelper::BindRayTracingShader(RHICommandQueueGraphics &queue, RDGPass *pass, RDGShader *ray_tracing_shader, const RDGShaderParamStructAndSizeInfo *info, const void *params) {
    if (!ray_tracing_shader->IsValid()) {
        MI_WARN("Shader {}: Invalid raytracing shader. Dispatch cancelled.",
            ray_tracing_shader->class_registry_->name);
        return false;
    }
    auto desc = SetupShaderParams(pass, ray_tracing_shader, queue, info, params);
    if (!desc.has_value()) {
        MI_WARN("Shader {}: Failed to upload shader parameters. Dispatch cancelled.",
            ray_tracing_shader->class_registry_->name);
        return false;
    }

    queue.BindPipeline(ray_tracing_shader->ray_tracing_pipeline_.Raw());
    queue.BindPipelineParameters(RHIBindPointType::kRayTracing, desc.value());
    auto sbt = ray_tracing_shader->GetSBTBuffers(queue);
    // Bind the shader binding table
    if (sbt.raygen && sbt.miss && sbt.hit) {
        queue.BindShaderBindingTable(sbt.raygen, sbt.miss, sbt.hit);
    } else {
        MI_WARN("Shader {}: Shader binding table is not set up correctly. Dispatch rays cancelled.",
            ray_tracing_shader->class_registry_->name);
        return false;
    }

    return true;
}


void RDGCommandHelper::Draw(RHICommandQueueGraphics &queue, RDGPass *pass, RDGShader *graphics_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void *params,
    int vertex_count, int instance_count, int first_vertex, int first_instance) {
    if (BindGraphicsShader(queue, pass, graphics_shader, info, params)) {
        queue.BeginRendering();
        queue.DrawPrimitive(vertex_count, instance_count, first_vertex, first_instance);
        queue.EndRendering();
    }
}

void RDGCommandHelper::Dispatch(RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params, uint32_t x, uint32_t y, uint32_t z) {
    if (BindComputeShader(queue, pass, shader, info, params)) {
        queue.Dispatch(x, y, z);
    } else {
        MI_WARN("Shader {}: Failed to upload shader parameters. Dispatch cancelled.",
            shader->class_registry_->name);
    }
}

void RDGCommandHelper::DispatchIndirect(RHICommandQueueGraphics &queue, RDGPass *pass, RDGShader *compute_shader,
    const RDGShaderParamStructAndSizeInfo *info, const void *params, RDGBuffer *indirect_buffer, uint32_t offset) {
    if (BindComputeShader(queue, pass, compute_shader, info, params)) {
        queue.DispatchIndirect(indirect_buffer->GetRHI().buffer, uint32_t(indirect_buffer->GetRHI().offset + offset));
    }
}

void RDGCommandHelper::DispatchRays(RHICommandQueueGraphics &queue, RDGPass *pass, RDGShader *ray_tracing_shader, const RDGShaderParamStructAndSizeInfo *info, const void *params, uint32_t width, uint32_t height, uint32_t depth) {
    if (BindRayTracingShader(queue, pass, ray_tracing_shader, info, params)) {
        queue.DispatchRays(width, height, depth);
    }
}

void RDGCommandHelper::DispatchRaysIndirect(RHICommandQueueGraphics &queue, RDGPass *pass, RDGShader *ray_tracing_shader, const RDGShaderParamStructAndSizeInfo *info, const void *params, RDGBuffer *indirect_buffer) {
    if (BindRayTracingShader(queue, pass, ray_tracing_shader, info, params)) {
        auto sbt = ray_tracing_shader->GetSBTBuffers(queue);
        queue.DispatchRaysIndirect(sbt.raygen, sbt.miss, sbt.miss_stride, sbt.hit, sbt.hit_stride, indirect_buffer->GetRHI());
    }
}



MI_NAMESPACE_END
