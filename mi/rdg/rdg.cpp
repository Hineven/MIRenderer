/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
//#include <corecrt_io.h>
#include <map>
#include <unordered_map>
#include <queue>
#include "rdg/rdg.h"

#include <rdg/rdg_param.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_pass_param_table.h>
#include <rhi/rhi_buffer.h>

#include "core/util/debug_prof.h"
#include "rdg/rdg_pass.h"
#include "rdg/rdg_pool.h"
#include "rdg/rdg_shader.h"
#include "rhi/rhi_types_string.h"

#ifndef NDEBUG
// Instantly start a command buffer submit after the execution of each pass.
// This is useful for debugging, but hurts performance alot.
// #define INSTANT_SUBMIT_FOR_EACH_PASS

// Enable extra validation checks during RDG execution.
#define RDG_DEBUG_VALIDATION
#endif


MI_NAMESPACE_BEGIN

static bool is_rdg_executing = false;

bool RDG_IsInRDGExecution () {
    return is_rdg_executing;
}

bool RDGProfilingContext::ResolveTimestampPeriods(std::vector<RDGTimePeriod> & out_periods,
    RHI & rhi,
    RHI::RHITimestampQueryMode mode) {
#if MI_ENABLE_TIMESTAMP
    out_periods.clear();

    if (marker_timestamps_.size() < 2) {
        return true;
    }

    auto marker_timestamp_results = rhi.QueryTimestamps(
        std::span<RHITimestamp*>(reinterpret_cast<RHITimestamp **>(marker_timestamps_.data()), marker_timestamps_.size()),
        mode
    );

    if (mode == RHI::RHITimestampQueryMode::kNonBlocking) {
        for (auto v : marker_timestamp_results) {
            if (v == UINT64_MAX) {
                // Not ready yet.
                return false;
            }
        }
    }

    uint64_t prev_time_ticks = marker_timestamp_results[0];

    auto valid_bits = std::min(rhi.GetDeviceProperties().timestamp_valid_bits, 64u);
    const uint64_t wrap_mod = (valid_bits == 64u) ? 0ull : (1ull << valid_bits);
    const float device_timestamp_tick_period = rhi.GetDeviceProperties().timestamp_period;

    const size_t n = marker_timestamps_.size();
    out_periods.reserve(n - 1);
    for (size_t i = 0; i < n - 1; i++) {
        uint64_t time_ticks = marker_timestamp_results[i + 1];
        uint64_t delta;
        if (wrap_mod != 0ull && time_ticks < prev_time_ticks) {
            delta = (wrap_mod - prev_time_ticks) + time_ticks;
        } else {
            delta = time_ticks - prev_time_ticks;
        }
        double duration = double(delta) * double(device_timestamp_tick_period) * 1e-9; // ns -> seconds
        prev_time_ticks = time_ticks;

        auto period = marker_periods_[i];
        period.duration = (float)duration;
        out_periods.emplace_back(std::move(period));
    }

    return true;
#else
    (void)out_periods;
    (void)rhi;
    (void)mode;
    return false;
#endif
}

RenderGraph::RenderGraph(const std::string & name): name_(name) {}
RenderGraph::~RenderGraph() {}

void RenderGraph::Execute (RDGResourcePool * pool, RHISyncPoint * sync_point) {
    DEBUG_PROFILE_SECTION(GraphExecSection);
    if (passes_.empty()) {
        MI_WARN("All graph passes are culled, nothing to execute.");
    }

    mi_assert(IsRenderThread(), "Only the render thread can execute RDG graphs.");
    mi_assert(RDG_IsInRDGExecution() == false, "Cannot execute RDG graph while another graph is executing. (which should be impossible!)");
    is_rdg_executing = true;

    std::set<RDGResource*> rdg_resources;

    // Prepare resource counters and compute lifetimes (first_pass / last_pass)
    // lifetime_map: RDGResource -> {first_pass, last_pass}
    struct ResourceLifetime { uint32_t first = UINT32_MAX; uint32_t last = 0; };
    std::unordered_map<RDGResource*, ResourceLifetime> lifetime_map;
    for (uint32_t pass_idx = 0; pass_idx < (uint32_t)passes_.size(); pass_idx++) {
        auto & e = passes_[pass_idx];
        for (auto & texture : e->compiled_.textures) {
            rdg_resources.insert(texture.texture.Raw());
            texture.texture->execution_ref_counter ++;
            auto & lf = lifetime_map[texture.texture.Raw()];
            if (lf.first == UINT32_MAX) lf.first = pass_idx;
            lf.last = pass_idx;
        }
        for (auto & buffer : e->compiled_.buffers) {
            rdg_resources.insert(buffer.buffer.Raw());
            buffer.buffer->execution_ref_counter ++;
            auto & lf = lifetime_map[buffer.buffer.Raw()];
            if (lf.first == UINT32_MAX) lf.first = pass_idx;
            lf.last = pass_idx;
        }
    }

    // Phase 1: Request allocations with lifetime info (dry run)
    for (auto & [resource, lf] : lifetime_map) {
        if (auto * texture = dynamic_cast<RDGTexture*>(resource)) {
            if (!texture->IsImported() && !texture->IsAllocated()) {
                pool->RequestAllocation(texture, lf.first, lf.last);
            }
        } else if (auto * buffer = dynamic_cast<RDGBuffer*>(resource)) {
            if (!buffer->IsImported() && !buffer->IsAllocated()) {
                pool->RequestAllocation(buffer, lf.first, lf.last);
            }
        }
    }

    // Phase 2: Commit all allocations (aliasing analysis + physical RHI allocation)
    pool->CommitAllocations();

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
                        MI_WARN("RDG: Unset / null UniformBuffer detected within pass {}, {}", pass->GetName(), ref.info->name);
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
            staging_buffer->Unmap();
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

    // Global parameter table creation: group passes by params_ptr, merge texture layouts across the group,
    // and create one shared parameter table per group.
    std::vector<RDGPassParameterTable> param_tables;
    {
        std::map<const void*, std::vector<RDGPass*>> param_groups;
        for (auto & pass : passes_) {
            if (pass->shader_ && pass->shader_param_data_) {
                param_groups[pass->shader_param_data_].push_back(pass.get());
            }
        }
        param_tables.reserve(param_groups.size());

        // Collect create-infos for all groups, then issue a single batched
        // CreateSignatureParameterTables call (one vkAllocateDescriptorSets + one
        // vkUpdateDescriptorSets for the whole frame) instead of one per group.
        // param_tables is reserved above, so &table pointers stay stable across emplace_back.
        std::vector<RHISignatureParameterTableCreateInfo> create_infos;
        std::vector<RDGPassParameterTable*> created_tables;

        for (auto & [params_ptr, group_passes] : param_groups) {
            if (group_passes.empty()) continue;
            auto * representative_pass = group_passes[0];
            auto * shader = representative_pass->shader_;
            auto * info = representative_pass->shader_param_struct_info_;
            if (!shader || !info) continue;

            auto & table = param_tables.emplace_back();
            table.params_ptr_ = params_ptr;
            table.root_signature_ = shader->GetRootSignature();
            table.info_ = info;

            // TODO optimize performance.
            // Merge texture layouts per binding slot across all passes in the group.
            auto MergeSlotLayout = [&](RHIPipelineResourceType type, auto & param_array) {
                for (const auto & [i, e] : std::views::enumerate(param_array)) {
                    auto tex = *static_cast<RDGShaderTextureParameter*>((void*)((uint8_t*)params_ptr + e.cpp_offset));
                    if (RDGParameter_IsUnsetPointer(tex.texture)) continue;

                    RHITextureLayoutType layout = RHITextureLayoutType::kUndefined;
                    for (auto * p : group_passes) {
                        for (auto & usage : p->compiled_.textures) {
                            if (usage.texture.Raw() == tex.texture) {
                                if(layout == RHITextureLayoutType::kUndefined) {
                                    layout = usage.layout;
                                } else if (layout != usage.layout) {
                                    // Conflicting layouts for the same binding slot across passes.
                                    // Regress to general layout when writing the descriptor.
                                    layout = RHITextureLayoutType::kGeneral;
                                    // FIXME overwrite pass usages to general layout as well for correct barrier placement.
                                    mi_assert(false, "Not implemented");
                                    break ;
                                } else layout = usage.layout;
                            }
                        }
                    }

                    auto key = RDGPassParameterTable::TextureSlotKey{type, (uint32_t)i};
                    auto it = table.merged_layouts_.find(key);
                    if (it != table.merged_layouts_.end()) {
                        if (it->second != layout) it->second = RHITextureLayoutType::kGeneral;
                    } else {
                        table.merged_layouts_[key] = layout;
                    }
                }
            };
            MergeSlotLayout(RHIPipelineResourceType::kUAV, info->uavs_);
            MergeSlotLayout(RHIPipelineResourceType::kSRV, info->srvs_);

            // Build parameter desc using the merged layouts.
            auto desc = RDGCommandHelper::BuildParameterDesc(cmd, table, this);
            if (desc) {
                create_infos.push_back({ table.root_signature_, *desc });
                created_tables.push_back(&table);
                // table.table_id_ is filled in below, once the contiguous id range is reserved.
                for (auto * pass : group_passes) {
                    pass->parameter_table_ = &table;
                }
            }
        }

        if (!create_infos.empty()) {
            // Reserve a contiguous id range so the batch command derives each id as base+i.
            uint32_t base_id = RDGCommandHelper::AllocateParameterTableIds((uint32_t)create_infos.size());
            for (size_t i = 0; i < created_tables.size(); i++) {
                created_tables[i]->table_id_ = base_id + (uint32_t)i;
            }
            // Copy create-infos into frame-local storage. The descriptor spans inside each desc
            // already point at frame memory allocated by BuildParameterDesc, so a shallow copy suffices
            // and the data stays valid until the command executes on the RHI thread.
            auto * storage = cmd.Allocate<RHISignatureParameterTableCreateInfo[]>(create_infos.size());
            for (size_t i = 0; i < create_infos.size(); i++) {
                storage[i] = create_infos[i];
            }
            cmd.CreateSignatureParameterTables(base_id, (uint32_t)create_infos.size(), storage);
        }
    }

    std::vector<RHITimestampRef> marker_timestamps;
    std::vector<RDGTimePeriod> marker_periods;
    RDGTimePeriod active_period;
    [[maybe_unused]] auto& rhi = RHI::Get();

    auto insert_timestamp = [&] () {
#if MI_ENABLE_TIMESTAMP
        auto timestamp = rhi.CreateTimestamp();
        if (timestamp) {
            marker_timestamps.push_back(timestamp);
            marker_periods.push_back(active_period);
            cmd.InsertTimestamp(timestamp.Raw());
        }
#else
        (void)active_period;
#endif
    };

    auto sync_active_period = [&] ([[maybe_unused]] RDGPass * pass, [[maybe_unused]] RHICommandQueueGraphics & queue) {
        insert_timestamp();
#if MI_ENABLE_TIMESTAMP
        auto curr_class_path = pass ? pass->class_path_ : std::vector<std::string>{};
        auto curr_pass_name = pass ? pass->GetName() : "";
        if (curr_class_path == active_period.class_names && curr_pass_name == active_period.pass_name) {
            return ;
        }
        size_t common_length = 0;
        while (common_length < std::min(active_period.class_names.size(), curr_class_path.size())) {
            if (active_period.class_names[common_length] != curr_class_path[common_length]) {
                break;
            }
            common_length ++;
        }
        if (!active_period.pass_name.empty()) queue.EndDebugMarker();
        for (size_t i = active_period.class_names.size(); i > common_length; i--) {
            queue.EndDebugMarker();
        }
        for (size_t i = common_length; i < curr_class_path.size(); i++) {
            queue.BeginDebugMarker(curr_class_path[i].c_str());
        }
        if (!curr_pass_name.empty()) queue.BeginDebugMarker(curr_pass_name.c_str());

        active_period.class_names = curr_class_path;
        active_period.pass_name = curr_pass_name;
#else
        (void)pass;
        (void)queue;
#endif
    };

    // RDG buffer corruption check for debugging
#ifdef RDG_DEBUG_VALIDATION
    auto IsRDGResourceCorrupted = [&] (RDGResource * resource) -> bool {
        if (!resource) return false;
        return !resource->IsCanaryAlive();
    };
#endif


    while (!ready_passes.empty()) {
#ifdef RDG_DEBUG_VALIDATION
        for (auto & validating_pass : passes_) {
            if (!validating_pass) continue ;
            for (auto & texture_use : validating_pass->compiled_.textures) {
                mi_assert(!IsRDGResourceCorrupted(texture_use.texture.Raw()),
                    "RDG Texture resource {} corruption for pass {} detected",
                    (void*)texture_use.texture.Raw(), validating_pass->name_.c_str());
            }
            for (auto & buffer_use : validating_pass->compiled_.buffers) {
                mi_assert(!IsRDGResourceCorrupted(buffer_use.buffer.Raw()),
                    "RDG Buffer resource {} corruption for pass {} detected",
                    (void*)buffer_use.buffer.Raw(), validating_pass->name_.c_str());
            }
        }
#endif
        int pass_index = ready_passes.front();
        ready_passes.pop();
        auto &pass = passes_[pass_index];
        // Mark incoming commands.
        sync_active_period(pass.get(), cmd);

        // Place resource barriers (combined textures + buffers).
        {
            // Gather texture barriers
            auto num_tex = pass->compiled_.textures.size();
            uint32_t num_tex_used = 0;
            auto textures = cmd.Allocate<RHITexture*[]>(num_tex);
            auto layouts = cmd.Allocate<RHITextureLayoutType[]>(num_tex);
            auto tex_src_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_tex);
            auto tex_dst_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_tex);
            auto tex_src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_tex);
            auto tex_dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_tex);
            for (const auto & texture_use : pass->compiled_.textures) {
                if (!texture_use.texture->GetRHI()) continue;

                auto prev_stages = texture_use.texture->GetReadStages() | texture_use.texture->GetWriteStages();
                auto prev_usage = texture_use.texture->GetReadAccess() | texture_use.texture->GetWriteAccess();
                auto prev_layout = texture_use.texture->GetCurrentLayout();

                // For aliasing: if this is the first use after allocation, inherit last access from the allocation.
                if (!prev_stages && texture_use.texture->allocation_) {
                    prev_stages = texture_use.texture->allocation_->last_read_stages | texture_use.texture->allocation_->last_write_stages;
                    prev_usage = texture_use.texture->allocation_->last_access;
                }

                RHITextureLayoutType target_layout = texture_use.layout;
                RHIGPUAccessFlags target_usage = texture_use.access;
                RHIPipelineStageFlags target_stages = texture_use.stages;

                if (texture_use.access == RHIGPUAccessFlagBits::kNone) continue ;

                bool needs_barrier = (prev_layout != target_layout);
                if (!needs_barrier && target_usage != RHIGPUAccessFlagBits::kNone) {
                    needs_barrier = prev_stages && (
                        (target_usage & RHIGPUAccessFlagBits::kWrite)
                        || ((target_usage & RHIGPUAccessFlagBits::kRead) && (prev_usage & RHIGPUAccessFlagBits::kWrite))
                    );
                }

                if (needs_barrier) {
                    textures[num_tex_used] = texture_use.texture->GetRHI();
                    tex_src_accesses[num_tex_used] = prev_usage;
                    tex_src_stages[num_tex_used] = prev_stages;
                    tex_dst_accesses[num_tex_used] = target_usage;
                    tex_dst_stages[num_tex_used] = target_stages;
                    layouts[num_tex_used] = target_layout;
                    num_tex_used ++;
                }
                
                texture_use.texture->Use(target_stages, target_usage, target_layout);
                
            }

            // Gather buffer barriers
            auto num_buf = pass->compiled_.buffers.size();
            auto num_buf_used = 0;
            auto buffers = cmd.Allocate<RHIBufferSpan[]>(num_buf);
            auto buf_src_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_buf);
            auto buf_dst_stages = cmd.Allocate<RHIPipelineStageFlags[]>(num_buf);
            auto buf_src_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_buf);
            auto buf_dst_accesses = cmd.Allocate<RHIGPUAccessFlags[]>(num_buf);
            for (const auto & buffer_use: pass->compiled_.buffers) {
                if (buffer_use.buffer->GetRHI()) {
                    if (buffer_use.access == RHIGPUAccessFlagBits::kNone) continue;
                    auto prev_stages = buffer_use.buffer->GetReadStages() | buffer_use.buffer->GetWriteStages();
                    auto curr_stages = buffer_use.stages;
                    auto prev_usage = buffer_use.buffer->GetReadAccess() | buffer_use.buffer->GetWriteAccess();
                    if (!prev_stages && buffer_use.buffer->allocation_) {
                        prev_stages = buffer_use.buffer->allocation_->last_read_stages | buffer_use.buffer->allocation_->last_write_stages;
                        prev_usage = buffer_use.buffer->allocation_->last_access;
                    }
                    auto curr_usage = buffer_use.access;
                    if (prev_stages && (
                        (curr_usage & RHIGPUAccessFlagBits::kWrite)
                        || ((curr_usage & RHIGPUAccessFlagBits::kRead) && (prev_usage & RHIGPUAccessFlagBits::kWrite)))) {
                        buffers[num_buf_used] = buffer_use.buffer->GetRHI();
                        buf_src_stages[num_buf_used] = prev_stages;
                        buf_dst_stages[num_buf_used] = curr_stages;
                        buf_src_accesses[num_buf_used] = prev_usage;
                        buf_dst_accesses[num_buf_used] = curr_usage;
                        num_buf_used ++;
                    }
                    buffer_use.buffer->Use(curr_stages, curr_usage);
                }
            }

            // Emit combined barriers
            if (num_tex_used || num_buf_used) {
                cmd.Barriers(
                    num_tex_used, textures, layouts, tex_src_stages, tex_dst_stages, tex_src_accesses, tex_dst_accesses,
                    num_buf_used, buffers, buf_src_stages, buf_dst_stages, buf_src_accesses, buf_dst_accesses
                );
            }
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

    sync_active_period(nullptr, cmd);

    cmd.EnqueueTranslateAndSubmit(sync_point, GetName());

#if MI_ENABLE_TIMESTAMP
    // Store profiling context for later resolution (typically next frame).
    profiling_context_.SafeRelease();
    if (!marker_timestamps.empty()) {
        profiling_context_.CreateIfNull();
        profiling_context_->marker_timestamps_ = std::move(marker_timestamps);
        profiling_context_->marker_periods_ = std::move(marker_periods);
    }
#endif

    // Release all uniform buffers as they are no longer needed.
    uniform_buffer_.SafeRelease();

    is_rdg_executing = false;
}

MI_NAMESPACE_END

