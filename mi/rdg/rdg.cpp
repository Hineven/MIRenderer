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
#include "rdg/rdg_pass.h"

MI_NAMESPACE_BEGIN

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

void RenderGraph::Execute (RDGResourcePool * pool) {

    // Directly use the graphics queue.
    auto & RHI = RHI::Get();
    auto & cmd = RHI.GetGraphicsCommandQueue();

    std::queue<int> ready_passes;
    for (int i = 0; i < (int)passes_.size(); i++) {
        if (num_pass_predecessors_[i] == 0) {
            ready_passes.push(i);
        }
    }

    // Write and upload all uniforms
    {
        auto staging_buffer = RHI.CreateBuffer(RHIBufferDesc{
            C::kRDGPoolUniformBufferBlockSize,
            RHIBufferUsageFlagBits::kTransferSrc | RHIBufferUsageFlagBits::kStaging
        });
        // TODO: is there a better implementation?
        for (auto pass : passes_) {
            pass->uniform_buffer_->RequestRHI(pool);ghjgh
        }

        // Write global uniform buffer
        void * mapped_uniform_buffer = (uint8_t*)shader_global_ub->GetRHI().buffer->Map() + shader_global_ub->GetRHI().offset;
        for (auto [i, e] : std::views::enumerate(base_info->global_uniforms_)) {
            FastTinyCopy((uint8_t*)mapped_uniform_buffer + e.shader_offset, (uint8_t*)params + e.cpp_offset, e.size);
        }
    }

    while (!ready_passes.empty()) {
        int pass_index = ready_passes.front();
        ready_passes.pop();
        auto &pass = passes_[pass_index];
        // Get resources ready
        for (auto texture_use : pass->used_textures_) {
            texture_use.texture->RequestRHI(pool);
        }
        for (auto buffer_use : pass->used_buffers_) {
            buffer_use.buffer->RequestRHI(pool);
        }
        // Place resource barriers.
        RHIPipelineStageFlags new_stages = pass->GetStageFlags();
        {
            auto textures = cmd.Allocate<RHITexture*[]>(pass->used_textures_.size());
            auto layouts = cmd.Allocate<RHITextureLayoutType[]>(pass->used_textures_.size());
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(pass->used_textures_.size());
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(pass->used_textures_.size());
            for (const auto & [i, texture_use] : std::views::enumerate(pass->used_textures_)) {
                textures[i] = texture_use.texture->GetRHI();
                auto & old_state = resource_accesses_[texture_use.texture->GetRHI()];
                src_accesses[i] = old_state.access;
                if (texture_use.usage == RDGPass::RDGTextureUsage::kShaderRead) {
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
            cmd.TextureBarriers((uint32_t)pass->used_textures_.size(), textures, layouts, new_stages, src_accesses, dst_accesses);
        }
        {
            auto buffers = cmd.Allocate<RHIBufferSpan[]>(pass->used_buffers_.size());
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(pass->used_buffers_.size());
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(pass->used_buffers_.size());
            for (const auto & [i, buffer_use] : std::views::enumerate(pass->used_buffers_)) {
                auto & old_state = resource_accesses_[buffer_use.buffer->GetRHI().buffer];
                buffers[i] = buffer_use.buffer->GetRHI();
                src_accesses[i] = old_state.access;
                RHIGPUAccessFlags access = {};
                switch (buffer_use.usage) {
                    case RDGPass::RDGBufferUsage::kUniformBuffer:
                    case RDGPass::RDGBufferUsage::kIndexBuffer:
                    case RDGPass::RDGBufferUsage::kVertexBuffer:
                    case RDGPass::RDGBufferUsage::kReadOnlyStorge:
                    case RDGPass::RDGBufferUsage::kIndirectBuffer:
                        access = RHIGPUAccessFlagBits::kRead;
                    break;
                    default:
                        access = RHIGPUAccessFlagBits::kAll;
                }
                dst_accesses[i] = access;
            }
            cmd.BufferBarriers((uint32_t)pass->used_buffers_.size(), buffers, new_stages, src_accesses, dst_accesses);
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

}
MI_NAMESPACE_END