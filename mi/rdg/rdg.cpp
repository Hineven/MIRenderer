/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg.h"

#include <corecrt_io.h>
#include <map>
#include <queue>
MI_NAMESPACE_BEGIN
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
        for (const auto & texture_use : pass->used_textures_) {
            auto & old_state = resource_accesses_[texture_use.texture.Raw()];
            if (texture_use.usage == RDGPass::RDGTextureUsage::kShaderRead) {
                cmd.TextureBarrier(
                    texture_use.texture->GetRHI(),
                    RHITextureLayoutType::kShaderReadOnlyOptimal,
                    new_stages,
                    old_state.access,
                    RHIGPUAccessFlagBits::kRead
                );
                // The barrier-chain rule allows us to replace the access mask with the new one.
                // For example, Write, Read, Read produces a W-R barrier and a R-R barrier.
                // The R-R barrier may be incorrect if it is standalone, but it is correct if it's following the
                // W-R barrier (chaining up, see Vulkan Barrier Chain).
                old_state.access = RHIGPUAccessFlagBits::kRead;
                old_state.stages = new_stages;
            } else if (texture_use.usage == RDGPass::RDGTextureUsage::kShaderReadWrite) {
                cmd.TextureBarrier(
                    texture_use.texture->GetRHI(),
                    RHITextureLayoutType::kGeneral,
                    new_stages,
                    old_state.access,
                    RHIGPUAccessFlagBits::kAll
                );
                old_state.access = RHIGPUAccessFlagBits::kAll;
                old_state.stages = new_stages;
            } else if (texture_use.usage == RDGPass::RDGTextureUsage::kOutputAttachment) {
                cmd.TextureBarrier(
                    texture_use.texture->GetRHI(),
                    RHITextureLayoutType::kColorAttachment,
                    new_stages,
                    old_state.access,
                    // Alpha blending may need reading the old value.
                    RHIGPUAccessFlagBits::kAll
                );
                old_state.access = RHIGPUAccessFlagBits::kAll;
                old_state.stages = new_stages;
            } else if (texture_use.usage == RDGPass::RDGTextureUsage::kDepthStencilAttachment) {
                cmd.TextureBarrier(
                    texture_use.texture->GetRHI(),
                    RHITextureLayoutType::kDepthStencilAttachment,
                    new_stages,
                    old_state.access,
                    RHIGPUAccessFlagBits::kAll
                );
                old_state.access = RHIGPUAccessFlagBits::kAll;
                old_state.stages = new_stages;
            } else {
                assert(false);
            }
        }
        for (const auto & buffer_use : pass->used_buffers_) {
            auto & old_state = resource_accesses_[buffer_use.buffer.Raw()];
            auto access =
            if (buffer_use.usage == RDGPass::RDGBufferUsage::kUniformBuffer) {
                cmd.BufferBarrier(
                    buffer_use.buffer->GetRHI(),
                    new_stages,
                    old_state.access,
                    RHIGPUAccessFlagBits::kRead
                );
                old_state.access = RHIGPUAccessFlagBits::kRead;
                old_state.stages = new_stages;
            } else if (buffer_use.usage == RDGPass::RDGBufferUsage::kReadOnlyStorge) {
                cmd.BufferBarrier(
                    buffer_use.buffer->GetRHI(),
                    new_stages,
                    old_state.access,
                    RHIGPUAccessFlagBits::kRead
                );
                old_state.access = RHIGPUAccessFlagBits::kRead;
                old_state.stages = new_stages;
            } else if (buffer_use.usage == RDGPass::RDGBufferUsage::kOutputAttachment) {
                cmd.BufferBarrier(
                    buffer_use.buffer->GetRHI(),
                    new_stages,
                    old_state.access,
                    RHIGPUAccessFlagBits::kAll
                );
                old_state.access = RHIGPUAccessFlagBits::kAll;
                old_state.stages = new_stages;
            } else {
                assert(false);
            }
        }
    }

}
MI_NAMESPACE_END