/*
 * Created: 2025/3/21
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_param.h>
#include <rdg/rdg_pass.h>
#include <rdg/rdg_pool.h>
#include <rdg/rdg_resource.h>
#include <rdg/rdg_shader.h>

MI_NAMESPACE_BEGIN

static RHIBindPipelineParametersDesc UploadShaderParams(
    RDGPass * pass, RDGShader * shader, RHICommandQueueGraphics & queue,
    const RDGShaderParamStructAndSizeInfo * base_info, const void * params) {
    RHIBindPipelineParametersDesc ret = {};
    // Bind uniform buffers
    bool has_globals = base_info->global_uniforms_.size() > 0;
    int num_ref_uniform_buffers = (int)base_info->uniform_buffers_.size();
    int num_uniform_buffers = num_ref_uniform_buffers + has_globals;
    if (num_uniform_buffers) {
        ret.uniforms = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(num_uniform_buffers), num_uniform_buffers);
        for (int i = 0; i < num_ref_uniform_buffers; i++) {
            auto struct_ptr = *(void**)((uint8_t*)params + base_info->uniform_buffers_[i].cpp_offset);
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUniformBuffer>((int)i);
            auto buffer_ptr = pass->GetGraph()->GetUniformBufferForParameterStruct(struct_ptr);
            auto span = buffer_ptr.buffer->GetRHI();
            span.offset += buffer_ptr.offset;
            ret.uniforms[i] = {span, slot};
        }
        if (has_globals) {
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUniformBuffer>(num_ref_uniform_buffers);
            auto buffer_ptr = pass->GetGraph()->GetUniformBufferForParameterStruct(params);
            auto span = buffer_ptr.buffer->GetRHI();
            span.offset += buffer_ptr.offset;
            ret.uniforms[num_ref_uniform_buffers] = {span, slot};
        }
    }
    // Bind storage buffers
    {
        ret.storages = std::span(queue.Allocate<RHIPipelineParameterBufferDesc[]>(base_info->storage_buffers_.size()), base_info->storage_buffers_.size());
        for (auto [i, e] : std::views::enumerate(base_info->storage_buffers_)) {
            auto buffer_ptr = *static_cast<RDGBuffer**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kStorageBuffer>((int)i);
            ret.storages[i] = {buffer_ptr->GetRHI(), slot};
        }
    }
    // Bind UAVs
    {
        ret.uavs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(base_info->uavs_.size()), base_info->uavs_.size());
        for (auto [i, e] : std::views::enumerate(base_info->uavs_)) {
            auto texture_ptr = *static_cast<RDGTexture**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUAVTexture>((int)i);
            ret.uavs[i] = {texture_ptr->GetRHI(), slot};
        }
    }
    // Bind SRVs
    {
        ret.srvs = std::span(queue.Allocate<RHIPipelineParameterTextureDesc[]>(base_info->srvs_.size()), base_info->srvs_.size());
        for (auto [i, e] : std::views::enumerate(base_info->srvs_)) {
            auto texture_ptr = *static_cast<RDGTexture**>((void*)((uint8_t*)params + e.cpp_offset));
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kSRVTexture>((int)i);
            ret.srvs[i] = {texture_ptr->GetRHI(), slot};
        }
    }
    // TODO bind samplers, as, etc...
    return ret;
}

void RDGCommandHelper::Dispatch(RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params, int x, int y, int z) {
    auto desc = UploadShaderParams(pass, shader, queue, info, params);
    queue.BindPipeline(shader->compute_pipeline_.Raw());
    queue.BindPipelineParameters(RHIBindPointType::kCompute, desc);
    queue.Dispatch(x, y, z);
}

void RDGCommandHelper::Draw(RHICommandQueueGraphics &queue, RDGPass *pass, RDGShader *graphics_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void *params,
    int vertex_count, int instance_count, int first_vertex, int first_instance) {
    auto desc = UploadShaderParams(pass, graphics_shader, queue, info, params);
    queue.BindPipeline(graphics_shader->graphics_pipeline_.Raw());
    queue.BindPipelineParameters(RHIBindPointType::kGraphics, desc);
    if (!info->vertex_buffers_.empty()) {
        for (auto e : info->vertex_buffers_) {
            auto ptr = (RDGBuffer*)*(void**)((uint8_t*)params + e.cpp_offset);
            if (ptr) {
                queue.BindVertexBuffer(
                    e.info->cpp_extra.vertex_buffer_info->index,
                    ((RDGBuffer*)*(void**)((uint8_t*)params + e.cpp_offset))->GetRHI()
                );
            } else {
                MI_LOG(MIInfraLogType::kWarning, "Shader {}: Null vertex buffer for paramter '{}'. Draw cancelled.",
                    graphics_shader->class_registry_->name, e.info->name);
                // cancel the draw
                return ;
            }
        }
    }
    RHIDrawDesc ds {};
    if (info->renderpass_.info) {
        auto pass_params = *(void**)((std::byte*)params + info->renderpass_.cpp_offset);
        auto pass_info = info->renderpass_.info->cpp_imported_struct_info.cpp_struct_info;
        for (auto [i, e] : std::views::enumerate(pass_info->render_targets_)) {
            auto param = *(RDGShaderRenderTargetParameter*)((uint8_t*)pass_params + e.cpp_offset);
            RHITexture * to_bound = nullptr;
            if (param.texture == nullptr) {
                MI_LOG(MIInfraLogType::kWarning, "Shader {}: Null render target for paramter '{}'. It will not be drawn.",
                    graphics_shader->class_registry_->name, e.info->name);
            } else {
                mi_warning(param.texture->GetDesc().usage & RHITextureUsageFlagBits::kRenderTarget,
                    "Shader {}: Assigned render target texture for {} is not created with kRenderTarget usage.",
                    graphics_shader->class_registry_->name, e.info->name);
                to_bound = param.texture->GetRHI();
            }
            ds.SetAttachment(
                e.info->cpp_extra.render_targets_info->target_index, to_bound,
                param.load_op, param.store_op, param.clear_value
            );
            if (e.info->cpp_extra.render_targets_info->target_index == -1) {
                ds.SetDepthStencilAttachment(to_bound, param.load_op, param.store_op, param.clear_value);
            }
        }
    }
    queue.UpdateDrawState(ds);
    // TODO multi draw in a single render pass support.
    queue.BeginRendering();
    queue.DrawPrimitive(vertex_count, instance_count, first_vertex, first_instance);\
    queue.EndRendering();
}

MI_NAMESPACE_END