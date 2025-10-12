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
#include "rdg/rdg_shader.h"
#include "rhi/rhi_types_string.h"

// Instantly start a command buffer submit after the execution of each pass.
// #define INSTANT_SUBMIT_FOR_EACH_PASS

MI_NAMESPACE_BEGIN

static bool is_rdg_executing = false;

bool RDG_IsInRDGExecution () {
    return is_rdg_executing;
}

RenderGraph::RenderGraph(const std::string & name): name_(name) {}
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

void RenderGraph::Execute (RDGResourcePool * pool, RHISyncPoint * sync_point) {

    if (passes_.empty()) {
        MI_WARN("All graph passes are culled, nothing to execute.");
    }

    mi_assert(IsRenderThread(), "Only the render thread can execute RDG graphs.");
    mi_assert(RDG_IsInRDGExecution() == false, "Cannot execute RDG graph while another graph is executing. (which should be impossible!)");
    is_rdg_executing = true;

    std::set<RDGResource*> rdg_resources;

    // Prepare resource counters
    for (auto & e : passes_) {
        for (auto & texture : e->compiled_.textures) {
            rdg_resources.insert(texture.texture.Raw());
            texture.texture->execution_ref_counter ++;
        }
        for (auto & buffer : e->compiled_.buffers) {
            rdg_resources.insert(buffer.buffer.Raw());
            buffer.buffer->execution_ref_counter ++;
        }
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
    // Create and upload all uniform buffers prior to all passes' executions. Also, inject usage to passes
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
                    // Reflect from shader and make sure that the UB is statically used.
                    // Otherwise, we skip it.
                    if (pass->shader_ && pass->shader_->QueryShaderAccess(ref.info->name).access == RHIGPUAccessFlagBits::kNone) {
                        continue ;
                    }
                    auto it = param_ptr_to_uniform_buffer_segment_.find(struct_ptr);
                    if (it == param_ptr_to_uniform_buffer_segment_.end()) {
                        // Missing uniform buffer, allocate a segment for it
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
                uniform_buffer_->GetRHI(), uniform_buffer_->GetReadStages() | uniform_buffer_->GetWriteStages(),
                RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite
            );
            uniform_buffer_->Use(RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferWrite);
            // Schedule the copy
            cmd.CopyBuffer(staging_buffer->GetSpan(), uniform_buffer_->GetRHI());
            // Insert a manual barrier
            // 25.7.21: Do not use ShaderRead access here. Use UniformRead instead. (ShaderRead does not include UniformRead)
            cmd.BufferBarrier(uniform_buffer_->GetRHI(), RHIPipelineStageFlagBits::kTransfer,
                // Uniform buffers are potentially used in graphics, compute and ray tracing stages.
                RHIPipelineStageFlagBits::kAllGraphics | RHIPipelineStageFlagBits::kCompute | RHIPipelineStageFlagBits::kRayTracing,
                RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kUniformRead);
            uniform_buffer_->Use(
                RHIPipelineStageFlagBits::kAllGraphics | RHIPipelineStageFlagBits::kCompute | RHIPipelineStageFlagBits::kRayTracing,
                RHIGPUAccessFlagBits::kUniformRead
            );
        }
        // No need for further adding the uniform buffer access to passes. 1 single barrier is enough.
    }

    std::string active_debug_marker_name;
    std::vector<RHITimestampRef> marker_timestamps;
    std::vector<std::string> marker_timestamp_names;
    [[maybe_unused]] auto& rhi = RHI::Get();

    auto insert_timestamp = [&] ([[maybe_unused]] int pass_index) {
#ifndef NDEBUG
        auto timestamp = rhi.CreateTimestamp();
        marker_timestamps.push_back(timestamp);
        marker_timestamp_names.push_back(
            pass_index >= 0 ? passes_[pass_index]->GetName() : ""
        );
        cmd.InsertTimestamp(timestamp.Raw());
#endif
    };

    while (!ready_passes.empty()) {
        int pass_index = ready_passes.front();
        ready_passes.pop();
        auto &pass = passes_[pass_index];
        // printf("================ Pass ================: %s\n", pass->name_.c_str());
        // Get resources ready in the pool
        for (const auto& texture_use : pass->compiled_.textures) {
            texture_use.texture->RequestRHI(pool);
        }
        for (const auto & buffer_use : pass->compiled_.buffers) {
            buffer_use.buffer->RequestRHI(pool);
        }
        // Add debug marker, group the passes with the same names
        if (!pass->name_.empty()) {
            if (pass->name_ != active_debug_marker_name) {
                if (!active_debug_marker_name.empty()) {
                    cmd.EndDebugMarker();
                }
                cmd.BeginDebugMarker(pass->name_.c_str());
                active_debug_marker_name = pass->name_;
                insert_timestamp(pass_index);
            }
        } else {
            if (!active_debug_marker_name.empty()) {
                cmd.EndDebugMarker();
                active_debug_marker_name.clear();
                insert_timestamp(pass_index);
            }
        }

        // Place resource barriers.
        // RHIPipelineStageFlags current_stages = pass->GetStageFlags();
        {
            auto num_barriers = pass->compiled_.textures.size();
            uint32_t num_barriers_used = 0;
            auto textures = cmd.Allocate<RHITexture*[]>(num_barriers);
            auto layouts = cmd.Allocate<RHITextureLayoutType[]>(num_barriers);
            auto src_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_barriers);
            auto dst_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_barriers);
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            for (const auto & texture_use : pass->compiled_.textures) {
                if (texture_use.texture->GetRHI()) {
                    auto prev_stages = texture_use.texture->GetReadStages() | texture_use.texture->GetWriteStages();
                    auto curr_stages = texture_use.stages;
                    auto prev_usage = texture_use.texture->GetReadAccess() | texture_use.texture->GetWriteAccess();
                    auto curr_usage = texture_use.access;
                    auto prev_layout = texture_use.texture->GetCurrentLayout();
                    auto curr_layout = texture_use.layout;
                    if (prev_layout != curr_layout || (prev_stages && (
                        curr_usage & RHIGPUAccessFlagBits::kWrite
                        || ((curr_usage & RHIGPUAccessFlagBits::kRead) && (prev_usage & RHIGPUAccessFlagBits::kWrite))))) {
                        textures[num_barriers_used] = texture_use.texture->GetRHI();
                        src_accesses[num_barriers_used] = prev_usage;
                        src_stages[num_barriers_used] = prev_stages;
                        dst_accesses[num_barriers_used] = curr_usage;
                        dst_stages[num_barriers_used] = curr_stages;
                        layouts[num_barriers_used] = curr_layout;
                        num_barriers_used ++;
                    }
                    texture_use.texture->Use(texture_use.stages, curr_usage, curr_layout);
                }
            }
            if (num_barriers_used) cmd.TextureBarriers(num_barriers_used, textures, layouts, src_stages, dst_stages, src_accesses, dst_accesses);
        }
        {
            auto num_barriers = pass->compiled_.buffers.size();
            auto num_barriers_used = 0;
            auto buffers = cmd.Allocate<RHIBufferSpan[]>(num_barriers);
            auto src_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_barriers);
            auto dst_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_barriers);
            auto src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            auto dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_barriers);
            for (const auto & buffer_use: pass->compiled_.buffers) {
                // printf("Barrier (RDG): %s\n", buffer_use.buffer->GetName().c_str());
                if (buffer_use.buffer->GetRHI()) {
                    auto prev_stages = buffer_use.buffer->GetReadStages() | buffer_use.buffer->GetWriteStages();
                    auto curr_stages = buffer_use.stages;
                    auto prev_usage = buffer_use.buffer->GetReadAccess() | buffer_use.buffer->GetWriteAccess();
                    auto curr_usage = buffer_use.access;
                    // printf("Barrrier buffer %s: %x %x\n", buffer_use.buffer->GetRHI().buffer->GetName(), (unsigned)prev_usage, (unsigned)curr_usage);
                    if (prev_stages && (
                        (curr_usage & RHIGPUAccessFlagBits::kWrite)
                        || ((curr_usage & RHIGPUAccessFlagBits::kRead) && (prev_usage & RHIGPUAccessFlagBits::kWrite)))) {
                        // printf("Actual barrier: %s %s %s %s\n", ToString(prev_stages).c_str(), ToString(curr_stages).c_str(), ToString(prev_usage).c_str(), ToString(curr_usage).c_str());
                        buffers[num_barriers_used] = buffer_use.buffer->GetRHI();
                        src_stages[num_barriers_used] = prev_stages;
                        dst_stages[num_barriers_used] = curr_stages;
                        src_accesses[num_barriers_used] = prev_usage;
                        dst_accesses[num_barriers_used] = curr_usage;
                        num_barriers_used ++;
                    }
                    buffer_use.buffer->Use(curr_stages, curr_usage);
                }
            }
            if (num_barriers_used) cmd.BufferBarriers(num_barriers_used, buffers, src_stages, dst_stages, src_accesses, dst_accesses);
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
        // Release resource counters
        for (auto & texture_use : pass->compiled_.textures) {
            if (texture_use.texture->GetRHI()) {
                texture_use.texture->execution_ref_counter --;
                if (texture_use.texture->execution_ref_counter == 0) {
                    assert(!(texture_use.texture->GetFlags() & RDGResourceFlagBits::kExport));
                    texture_use.texture->ReleaseRHI();
                }
            }
        }
        for (auto & buffer_use : pass->compiled_.buffers) {
            if (buffer_use.buffer->GetRHI()) {
                buffer_use.buffer->execution_ref_counter --;
                if (buffer_use.buffer->execution_ref_counter == 0) {
                    assert(!(buffer_use.buffer->GetFlags() & RDGResourceFlagBits::kExport));
                    buffer_use.buffer->ReleaseRHI();
                }
            }
        }
        fflush(stdout);
#ifdef INSTANT_SUBMIT_FOR_EACH_PASS
        cmd.EnqueueTranslateAndSubmit(nullptr, pass->GetName());
#endif
        // Release the pass (and decrement the reference count of the resources its holding)
        pass.reset();
    }

    // End the debug marker if it is still active
    if (!active_debug_marker_name.empty()) {
        cmd.EndDebugMarker();
        insert_timestamp(-1);
    }

    cmd.EnqueueTranslateAndSubmit(sync_point, GetName());

#ifndef NDEBUG
    // Extract timestamp results
    if (!marker_timestamps.empty()) {
        timestamp_periods_.clear();
        uint64_t prev_time_ticks = 0;
        if (!marker_timestamps.empty())
            prev_time_ticks = marker_timestamps[0]->QueryTimestamp(); // This function implicitly waits for the GPU to finish.
        for (size_t i = 0; i < marker_timestamps.size() - 1; i++) {
            uint64_t time_ticks = marker_timestamps[i + 1]->QueryTimestamp();

            uint64_t delta = time_ticks - prev_time_ticks;
            float period = rhi.GetDeviceProperties().timestamp_period;
            double duration = double(delta) * double(period) * 1e-9; // ns
            prev_time_ticks = time_ticks;

            timestamp_periods_.emplace_back(marker_timestamp_names[i], duration);
        }
    }
#endif

    // Release all uniform buffers as they are no longer needed.
    uniform_buffer_.SafeRelease();

    is_rdg_executing = false;
}

MI_NAMESPACE_END