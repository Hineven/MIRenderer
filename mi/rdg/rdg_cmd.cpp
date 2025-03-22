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

static RHIBindPipelineParametersDesc UploadShaderParams(RDGPass * pass, RDGShader * shader, RHICommandQueueGraphics & queue, const RDGShaderParamStructAndSizeInfo * base_info, void * params) {
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
            ret.uniforms[i] = {buffer_ptr->GetRHI(), slot};
        }
        if (has_globals) {
            uint32_t slot = shader->ConvertParamResourceIndexToResourceSlot<RHIParamType::kUniformBuffer>(num_ref_uniform_buffers);
            auto buffer_ptr = pass->GetGraph()->GetUniformBufferForParameterStruct(params);
            ret.uniforms[num_ref_uniform_buffers] = {buffer_ptr->GetRHI(), slot};
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
    // TODO Bind samplers, etc...
    return ret;
}

void RDGCommandHelper::Dispatch(RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * shader,
    const RDGShaderParamStructAndSizeInfo * info, void * params, int x, int y, int z) {
    auto desc = UploadShaderParams(pass, shader, queue, info, params);
    queue.BindPipeline(shader->compute_pipeline_.Raw());
    queue.BindPipelineParameters(RHIBindPointType::kCompute, desc);
    queue.Dispatch(x, y, z);
}


MI_NAMESPACE_END