/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <corecrt_io.h>
#include <map>
#include <queue>
#include "rdg/rdg.h"

#include <rdg/rdg_param.h>

#include "rdg/rdg_pass.h"
#include "rdg/rdg_pool.h"

MI_NAMESPACE_BEGIN

RenderGraph::~RenderGraph() {
    printf("RDG destruction\n");
}

FORCEINLINE static void FastTinyCopy (void* __restrict dst, const void* __restrict src, size_t size) {
    switch (size) {
        case 4: *static_cast<uint32_t*>(dst) = *static_cast<const uint32_t*>(src); break;
        case 8: *static_cast<uint64_t*>(dst) = *static_cast<const uint64_t*>(src); break;
        case 12: {
            const uint32_t* s = static_cast<const uint32_t*>(src);
            uint32_t* d = static_cast<uint32_t*>(dst);
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            break;
        }
        case 16: {
            const uint64_t* s = static_cast<const uint64_t*>(src);
            uint64_t* d = static_cast<uint64_t*>(dst);
            d[0] = s[0];
            d[1] = s[1];
            break;
        }
        default: std::memcpy(dst, src, size);
    }
}

void RenderGraph::Execute (RDGResourcePool * pool, RHISyncPoint * sync_point) {


    // Directly use the graphics queue.
    auto & RHI = RHI::Get();
    auto & cmd = RHI.GetGraphicsCommandQueue();

    std::queue<int> ready_passes;
    for (int i = 0; i < (int)passes_.size(); i++) {
        if (num_pass_predecessors_[i] == 0) {
            ready_passes.push(i);
        }
    }
    // Prepare and upload all uniforms
    {
        auto WriteUniforms = [&] (RDGBuffer* buffer, const RDGShaderParamStructAndSizeInfo * param_info, const void * param_data) {
            if (buffer && !buffer->IsAllocated()) {
                buffer->RequestRHI(pool);
                auto ptr = buffer->GetStagingMappedPtr();
                for (auto e : param_info->global_uniforms_) {
                    FastTinyCopy((std::byte*)ptr + e.shader_offset, (std::byte*)param_data + e.cpp_offset, e.size);
                }
            }
        };
        for (auto & pass : passes_) {
            // Generic passes have no shader parameters and thus no need to upload uniforms.
            if (pass->shader_param_struct_info_) {
                {
                    auto it = ref_buffer_map_.find(pass->shader_param_data_);
                    if (it == ref_buffer_map_.end()) {
                        // Create and write to the global buffer if not present
                        auto buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kUniform, pass->shader_param_struct_info_->size);
                        WriteUniforms(buffer.Raw(), pass->shader_param_struct_info_, pass->shader_param_data_);
                        resource_accesses_[buffer->GetRHI().buffer] = {RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kWrite};
                        ref_buffer_map_[pass->shader_param_data_] = std::move(buffer);
                    }
                }
                // Also recursively request all the uniform buffers referenced
                for (auto ref : pass->shader_param_struct_info_->uniform_buffers_) {
                    auto struct_ptr = *(void**)((std::byte*)pass->shader_param_data_ + ref.cpp_offset);
                    auto it = ref_buffer_map_.find(struct_ptr);
                    if (it == ref_buffer_map_.end()) {
                        auto buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kUniform, ref.info->cpp_imported_struct_info.cpp_struct_info->size);
                        WriteUniforms(buffer.Raw(), ref.info->cpp_imported_struct_info.cpp_struct_info, struct_ptr);
                        resource_accesses_[buffer->GetRHI().buffer] = {RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kWrite};
                        ref_buffer_map_[struct_ptr] = std::move(buffer);
                    }
                }
            }
        }
        // Copy staging buffers to uniform buffers
        pool->StageUniformBuffers(cmd);
    }

    while (!ready_passes.empty()) {
        int pass_index = ready_passes.front();
        ready_passes.pop();
        auto &pass = passes_[pass_index];
        // Get resources ready
        for (auto texture_use : pass->compiled_.used_textures) {
            texture_use.texture->RequestRHI(pool);
        }
        for (auto buffer_use : pass->compiled_.used_buffers) {
            buffer_use.buffer->RequestRHI(pool);
        }
        // Place resource barriers.
        RHIPipelineStageFlags new_stages = pass->GetStageFlags();
        {
            auto num_barriers = pass->compiled_.used_textures.size();
            auto textures = cmd.Allocate<RHITexture*[]>(num_barriers);
            auto layouts = cmd.Allocate<RHITextureLayoutType[]>(num_barriers);
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            for (const auto & [i, texture_use] : std::views::enumerate(pass->compiled_.used_textures)) {
                textures[i] = texture_use.texture->GetRHI();
                auto & old_state = resource_accesses_[texture_use.texture->GetRHI()];
                src_accesses[i] = old_state.access;
                if (texture_use.usage == RDGPass::RDGTextureUsage::kTransferSrc) {
                    layouts[i] = RHITextureLayoutType::kTransferSrcOptimal;
                    dst_accesses[i] = RHIGPUAccessFlagBits::kRead;
                } else if (texture_use.usage == RDGPass::RDGTextureUsage::kTransferDst) {
                    layouts[i] = RHITextureLayoutType::kTransferDstOptimal;
                    dst_accesses[i] = RHIGPUAccessFlagBits::kWrite;
                } else if (texture_use.usage == RDGPass::RDGTextureUsage::kShaderRead) {
                    layouts[i] = RHITextureLayoutType::kShaderReadOnlyOptimal;
                    dst_accesses[i] = RHIGPUAccessFlagBits::kRead;
                    // The barrier-chain rule allows us to replace the access mask with the new one.
                    // For example, Write, Read, Read produces a W-R barrier and a R-R barrier.
                    // The R-R barrier may be incorrect if it is standalone, but it is correct if it's following the
                    // W-R barrier (chaining up, see Vulkan Barrier Chain).
                } else if (texture_use.usage == RDGPass::RDGTextureUsage::kShaderReadWrite) {
                    layouts[i] = RHITextureLayoutType::kGeneral;
                    dst_accesses[i] = RHIGPUAccessFlagBits::kAll;
                } else if (texture_use.usage == RDGPass::RDGTextureUsage::kOutputAttachment) {
                    layouts[i] = RHITextureLayoutType::kColorAttachment;
                    // Read for (potentially) alpha blending
                    dst_accesses[i] = RHIGPUAccessFlagBits::kAll;
                } else if (texture_use.usage == RDGPass::RDGTextureUsage::kDepthStencilAttachment) {
                    layouts[i] = RHITextureLayoutType::kDepthStencilAttachment;
                    dst_accesses[i] = RHIGPUAccessFlagBits::kAll;
                } else {
                    assert(false);
                }
                old_state.access = dst_accesses[i];
                old_state.stages = new_stages;
            }
            cmd.TextureBarriers((uint32_t)num_barriers, textures, layouts, new_stages, src_accesses, dst_accesses);
        }
        {
            auto num_barriers = pass->compiled_.used_buffers.size();
            auto buffers = cmd.Allocate<RHIBufferSpan[]>(num_barriers);
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            for (const auto & [i, buffer_use] : std::views::enumerate(pass->compiled_.used_buffers)) {
                auto & old_state = resource_accesses_[buffer_use.buffer->GetRHI().buffer];
                buffers[i] = buffer_use.buffer->GetRHI();
                src_accesses[i] = old_state.access;
                old_state.access = buffer_use.access;
                dst_accesses[i] = buffer_use.access;
            }
            cmd.BufferBarriers((uint32_t)num_barriers, buffers, new_stages, src_accesses, dst_accesses);
        }
        // Execute the pass
        pass->pass_(pass.get(), cmd);
        // Mark the pass as executed
        for (int e = pass_node_heads_[pass_index]; e != -1; e = edges_[e].next_edge) {
            const auto & edge = edges_[e];
            if (! (--num_pass_predecessors_[edge.dst_pass_index])) {
                // All predecessors are executed, queue it up for execution.
                ready_passes.push(edge.dst_pass_index);
            }
        }
        // Release the pass (and decrement the reference count of the resources its holding)
        pass.reset();
    }
    cmd.EnqueueTranslateAndSubmit(sync_point);
    // Release referenced RDG buffers
    ref_buffer_map_.clear();

}
MI_NAMESPACE_END