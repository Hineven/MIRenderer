/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
//#include <corecrt_io.h>
#include <map>
#include <queue>
#include "rdg/rdg.h"

#include <rdg/rdg_param.h>
#include <rhi/rhi_buffer.h>

#include "rdg/rdg_pass.h"
#include "rdg/rdg_pool.h"

MI_NAMESPACE_BEGIN

RenderGraph::~RenderGraph() {}

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

static RHIGPUAccessFlags GetTextureUsageAccess (RDGTextureUsageType usage) {
    if (usage == RDGTextureUsageType::kDontCare) {
        return RHIGPUAccessFlagBits::kAll;
    } else if (usage == RDGTextureUsageType::kTransferSrc) {
        return RHIGPUAccessFlagBits::kRead;
    } else if (usage == RDGTextureUsageType::kTransferDst) {
        return RHIGPUAccessFlagBits::kWrite;
    } else if (usage == RDGTextureUsageType::kShaderRead) {
        // The barrier-chain rule allows us to replace the access mask with the new one.
        // For example, Write, Read, Read produces a W-R barrier and a R-R barrier.
        // The R-R barrier may be incorrect if it is standalone, but it is correct if it's following the
        // W-R barrier (chaining up, see Vulkan Barrier Chain).
        return RHIGPUAccessFlagBits::kRead;
    } else if (usage == RDGTextureUsageType::kShaderReadWrite) {
        return RHIGPUAccessFlagBits::kAll;
    } else if (usage == RDGTextureUsageType::kOutputAttachment) {
        // Read for (potentially) alpha blending
        return RHIGPUAccessFlagBits::kAll;
    } else if (usage == RDGTextureUsageType::kDepthStencilAttachment) {
        return RHIGPUAccessFlagBits::kAll;
    } else if (usage == RDGTextureUsageType::kNone) {
        return RHIGPUAccessFlagBits::kNone;
    } else {
        assert(false);
        return RHIGPUAccessFlagBits::kNone; // Default return to avoid compiler warnings
    }
}

static RHITextureLayoutType GetTextureLayout (RDGTextureUsageType usage) {
    if (usage == RDGTextureUsageType::kDontCare) {
        return RHITextureLayoutType::kUndefined;
    } else if (usage == RDGTextureUsageType::kTransferSrc) {
        return RHITextureLayoutType::kTransferSrcOptimal;
    } else if (usage == RDGTextureUsageType::kTransferDst) {
        return RHITextureLayoutType::kTransferDstOptimal;
    } else if (usage == RDGTextureUsageType::kShaderRead) {
        return RHITextureLayoutType::kShaderReadOnlyOptimal;
    } else if (usage == RDGTextureUsageType::kShaderReadWrite) {
        return RHITextureLayoutType::kGeneral;
    } else if (usage == RDGTextureUsageType::kOutputAttachment) {
        return RHITextureLayoutType::kColorAttachment;
    } else if (usage == RDGTextureUsageType::kDepthStencilAttachment) {
        return RHITextureLayoutType::kDepthStencilAttachment;
    } else if (usage == RDGTextureUsageType::kNone) {
        return RHITextureLayoutType::kUndefined;
    } else {
        assert(false);
        return RHITextureLayoutType::kUndefined; // Default return to avoid compiler warnings
    }
}

void RenderGraph::Execute (RDGResourcePool * pool, RHISyncPoint * sync_point) {

    if (passes_.size() == 0) {
        MI_WARN("All graph passes are culled, nothing to execute.");
    }

    // Directly use the graphics queue.
    auto & RHI = RHI::Get();
    auto & cmd = RHI.GetGraphicsCommandQueue();

    std::queue<int> ready_passes;
    for (int i = 0; i < (int)passes_.size(); i++) {
        if (num_pass_predecessors_[i] == 0) {
            ready_passes.push(i);
        }
    }
    // Uniform buffers are handled upon pass execution
    // Create and upload all uniform buffers. Also, inject usage to passes
    {
        size_t all_uniform_buffer_size = 0;
        auto WriteUniforms = [&] (void * ptr, const RDGShaderParamInfo * param_info, const void * param_data) {
            memcpy(ptr, param_data, param_info->size);
        };
        for (auto & pass : passes_) {
            if (pass->shader_param_struct_info_) { // Valid for non-generic passes
                // Also recursively request all the uniform buffers referenced
                for (auto ref : pass->shader_param_struct_info_->uniform_buffers_) {
                    auto struct_ptr = *(void**)((std::byte*)pass->shader_param_data_ + ref.cpp_offset);
                    if (struct_ptr == nullptr || RDGParameter_IsUnsetPointer(struct_ptr)) {
                        continue ;
                    }
                    auto it = param_ptr_to_uniform_buffer_segment_.find(struct_ptr);
                    if (it == param_ptr_to_uniform_buffer_segment_.end()) {
                        auto aligned_size = RoundUp(ref.info->size, C::kUniformBufferAlignment);
                        param_ptr_to_uniform_buffer_segment_[struct_ptr] = {
                            all_uniform_buffer_size,
                            ref.info
                        };
                        all_uniform_buffer_size += aligned_size;
                    }
                }
            }
        }

        // Staging buffer as well, here we directly create from RHI because it relates to GPU-CPU synchronization,
        // and thus it should not reside in the RDG resource pool for further reusing and recycling.
        // RHI layer recycling mechanism will take care of it.
        if (all_uniform_buffer_size) {
            // Batch allocate all uniform buffers
            uniform_buffer_ = RDGBuffer::Create(RHIBufferUsageFlagBits::kUniform, all_uniform_buffer_size);
            uniform_buffer_->SetName("UniformBuffer");

            auto staging_buffer = RHI::Get().CreateBuffer(
                all_uniform_buffer_size, RHIBufferUsageFlagBits::kStaging | RHIBufferUsageFlagBits::kTransferSrc);
            uniform_buffer_->RequestRHI(pool);
            // Write to staging buffer
            auto staging_ptr = staging_buffer->Map();
            for (auto & [ptr, desc] : param_ptr_to_uniform_buffer_segment_) {
                WriteUniforms((std::byte*)staging_ptr + desc.offset, desc.param_info, ptr);
            }
            // Barrier the uniform buffer
            cmd.BufferBarrier(
                uniform_buffer_->GetRHI(), RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kWrite
            );
            // Schedule the copy
            cmd.CopyBuffer(staging_buffer->GetSpan(), uniform_buffer_->GetRHI());
            // Insert a manual barrier
            cmd.BufferBarrier(uniform_buffer_->GetRHI(), RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kRead);
        }
        // No need for further adding the uniform buffer access to passes. 1 single barrier is enough.
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
        // Add debug marker
        if (!pass->name_.empty()) {
            cmd.BeginDebugMarker(pass->name_.c_str());
        }

        // Place resource barriers.
        RHIPipelineStageFlags new_stages = pass->GetStageFlags();
        {
            auto num_barriers = pass->compiled_.used_textures.size();
            uint32_t num_barriers_used = 0;
            auto textures = cmd.Allocate<RHITexture*[]>(num_barriers);
            auto layouts = cmd.Allocate<RHITextureLayoutType[]>(num_barriers);
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            for (const auto & texture_use : pass->compiled_.used_textures) {
                if (texture_use.texture->GetRHI()) {
                    textures[num_barriers_used] = texture_use.texture->GetRHI();
                    src_accesses[num_barriers_used] = GetTextureUsageAccess(texture_use.texture->GetLastUsage());
                    layouts[num_barriers_used] = GetTextureLayout(texture_use.usage);
                    dst_accesses[num_barriers_used] = GetTextureUsageAccess(texture_use.usage);
                    texture_use.texture->Use(texture_use.usage);
                    num_barriers_used ++;
                }
            }
            cmd.TextureBarriers(num_barriers_used, textures, layouts, new_stages, src_accesses, dst_accesses);
        }
        {
            auto num_barriers = pass->compiled_.used_buffers.size();
            auto num_barriers_used = 0;
            auto buffers = cmd.Allocate<RHIBufferSpan[]>(num_barriers);
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            for (const auto & buffer_use: pass->compiled_.used_buffers) {
                if (buffer_use.buffer->GetRHI()) {
                    buffers[num_barriers_used] = buffer_use.buffer->GetRHI();
                    src_accesses[num_barriers_used] = buffer_use.buffer->GetLastUsage();
                    dst_accesses[num_barriers_used] = buffer_use.access;
                    buffer_use.buffer->Use(buffer_use.access);
                    num_barriers_used ++;
                }
            }
            cmd.BufferBarriers(num_barriers_used, buffers, new_stages, src_accesses, dst_accesses);
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
        // End debug marker
        if (!pass->name_.empty()) {
            cmd.EndDebugMarker();
        }
        // Release the pass (and decrement the reference count of the resources its holding)
        pass.reset();
    }
    cmd.EnqueueTranslateAndSubmit(sync_point);
}
MI_NAMESPACE_END