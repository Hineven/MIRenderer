/*
 * Created: 2025/3/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <queue>
#include <ranges>

#include "rdg/rdg_pass.h"
#include "rdg/rdg_builder.h"
#include "rhi/rhi_buffer.h"

#include "rdg/rdg.h"
#include "rhi/rhi_as.h"

MI_NAMESPACE_BEGIN
    RenderGraphBuilder::RenderGraphBuilder () {
    allocator_ = std::make_unique<TOneTimeLinearAllocator<>>();
}

RenderGraphBuilder::~RenderGraphBuilder() {}


RDGPass * RenderGraphBuilder::AddPass(
    const char *name,
    RDGPassType pass_type,
    RDGPassFlags pass_flags,
    RDGShader * shader,
    const RDGShaderParamStructAndSizeInfo *shader_param_struct_info,
    void *parameter_struct,
    RDGPassLambda && pass_lambda
) {
    auto ptr = new RDGPass(
            name,
            current_pass_index_ ++,
            pass_type,
            pass_flags,
            std::move(pass_lambda)
    );
    ptr->shader_ = shader;
    ptr->shader_param_struct_info_ = shader_param_struct_info;
    ptr->shader_param_data_ = parameter_struct;
#ifndef NDEBUG
    if (shader_param_struct_info && parameter_struct) {
        for (auto e : shader_param_struct_info->uniform_buffers_) {
            auto struct_ptr = *(void**)((uint8_t*)parameter_struct + e.cpp_offset);
            if (!RDGParameter_IsUnsetPointer(struct_ptr) && struct_ptr) {
                uint32_t crc = CRC32(struct_ptr, e.info->size);
                auto it = param_struct_ptr_to_data_crc.find(struct_ptr);
                if (it != param_struct_ptr_to_data_crc.end()) {
                    if (it->second != crc) {
                        MI_LOG(MIInfraLogType::kError,
                            "Pass {}: Parameter struct (name {}, pointer {}) has different CRC values among different passes."
                            "This may indicate that you are reusing a uniform buffer struct as well as modifying its contents"
                            "in-between different passes. This will cause undefined behavior as RDG identifies paramter structs"
                            "solely by their pointers. (old {}, new {})",
                            name, e.info->name, struct_ptr,
                            it->second, crc);
                    }
                } else {
                    param_struct_ptr_to_data_crc[struct_ptr] = crc;
                }
            }
        }
    }
#endif
    auto pass = std::unique_ptr<RDGPass>(ptr);
    // Compile the pass, to keep references to RDG resources alive
    pass->PreCompile();
    // Add to the pass list
    passes_.push_back(std::move(pass));
    return ptr;
}

TRef<RDGTexture> RenderGraphBuilder::CreateTexture(RHITextureDesc desc) {
    return RDGTexture::Create(desc);
}

TRef<RDGBuffer> RenderGraphBuilder::CreateBuffer (RHIBufferUsageFlags usage, size_t size, bool dedicated, bool no_warning) {
    return RDGBuffer::Create(usage, size, dedicated, no_warning);
}

RDGBuffer * RenderGraphBuilder::Import(RHIBuffer *resource, RHIGPUAccessFlags prev_access, RHIPipelineStageFlags prev_stages) {
    mi_assert(resource != nullptr, "Importing null buffer resource.");
    auto it = external_buffer_map_.find(resource);
    if (it != external_buffer_map_.end()) {
        auto ret = external_buffer_map_[resource];
        return ret.Raw();
    }
    auto desc = resource->GetDesc();
    auto buffer_raw_ptr = new RDGBuffer(desc.size, desc);
    auto buffer = TRef<RDGBuffer>(buffer_raw_ptr);
    buffer->rhi_buffer_span_ = resource->GetSpan();
    buffer->read_access_ = prev_access & RHIGPUAccessFlagBits::kRead;
    buffer->write_access_ = prev_access & RHIGPUAccessFlagBits::kWrite;
    buffer->read_stages_ = (prev_access & RHIGPUAccessFlagBits::kRead) ? prev_stages : RHIPipelineStageFlagBits::kNone;
    buffer->write_stages_ = (prev_access & RHIGPUAccessFlagBits::kWrite) ? prev_stages : RHIPipelineStageFlagBits::kNone;
    buffer->flags_ = RDGResourceFlagBits::kImported | RDGResourceFlagBits::kExport;
    {
        auto original_name = resource->GetName();
        if (original_name) buffer->SetName(original_name);
    }
    external_buffer_map_[resource] = buffer;
    return buffer.Raw();
}

RDGTexture * RenderGraphBuilder::Import(RHITexture * resource, RHITextureLayoutType prev_layout, RHIGPUAccessFlags prev_access, RHIPipelineStageFlags prev_stages) {
    mi_assert(resource != nullptr, "Importing null texture resource.");
    auto it = external_texture_map_.find(resource);
    if (it != external_texture_map_.end()) {
        auto ret = external_texture_map_[resource];
        return ret.Raw();
    }
    mi_assert(resource != nullptr, "Cannot import a null texture.");
    auto texture_raw_ptr = new RDGTexture(resource->GetDesc());
    auto texture = TRef<RDGTexture>(texture_raw_ptr);
    texture->rhi_texture_ = resource;
    texture->read_access_ = prev_access & RHIGPUAccessFlagBits::kRead;
    texture->write_access_ = prev_access & RHIGPUAccessFlagBits::kWrite;
    texture->read_stages_ = (prev_access & RHIGPUAccessFlagBits::kRead) ? prev_stages : RHIPipelineStageFlagBits::kNone;
    texture->write_stages_ = (prev_access & RHIGPUAccessFlagBits::kWrite) ? prev_stages : RHIPipelineStageFlagBits::kNone;
    texture->current_layout_ = prev_layout;
    texture->flags_ = RDGResourceFlagBits::kImported | RDGResourceFlagBits::kExport;
    {
        auto original_name = resource->GetName();
        if (original_name) texture->SetName(original_name);
    }
    external_texture_map_[resource] = texture;
    return texture.Raw();
}



TRef<RenderGraph> RenderGraphBuilder::Compile(const std::string & graph_name) {
    // Compile all passes first
    for (auto & e : passes_) {
        e->Compile();
    }
    std::map<void*, std::vector<RDGPass*>> in_resource_pass_map;
    std::map<void*, std::vector<RDGPass*>> out_resource_pass_map;
    std::vector<std::unique_ptr<RDGPass>> culled_passes;
    std::vector<int> culled_pass_heads, pass_heads_rev;
    culled_pass_heads.resize(passes_.size(), -1);
    pass_heads_rev.resize(passes_.size(), -1);
    std::vector<RenderGraph::Edge> culled_edges, edges_rev;
    auto AddEdge = [&](int from, int to) {
        int edge_index = (int)culled_edges.size();
        culled_edges.emplace_back(from, to, culled_pass_heads[from]);
        culled_pass_heads[from] = edge_index;
    };
    auto AddEdgeRev = [&](int from, int to) {
        int edge_index_rev = (int)edges_rev.size();
        edges_rev.emplace_back(from, to, pass_heads_rev[from]);
        pass_heads_rev[from] = edge_index_rev;
    };
    for(auto & pass : passes_) {
        // Find execution dependencies (to prior passes)
        std::vector<RDGPass*> dependencies;
        // W-R
        for(auto & in_texture : pass->compiled_.in_textures) {
            for(auto & out_pass : out_resource_pass_map[in_texture]) {
                dependencies.push_back(out_pass);
            }
        }
        for (auto & in_buffer : pass->compiled_.in_buffers) {
            for (auto & out_pass : out_resource_pass_map[in_buffer]) {
                dependencies.push_back(out_pass);
            }
        }
        for (auto & in_as : pass->compiled_.in_acceleration_structures) {
            for (auto & out_pass : out_resource_pass_map[in_as]) {
                dependencies.push_back(out_pass);
            }
        }
        // RW-W
        for(auto & out_texture : pass->compiled_.out_textures) {
            for(auto & in_pass : in_resource_pass_map[out_texture]) {
                dependencies.push_back(in_pass);
            }
            for (auto & out_pass : out_resource_pass_map[out_texture]) {
                dependencies.push_back(out_pass);
            }
        }
        for(auto & out_buffer : pass->compiled_.out_buffers) {
            for(auto & in_pass : in_resource_pass_map[out_buffer]) {
                dependencies.push_back(in_pass);
            }
            for (auto & out_pass : out_resource_pass_map[out_buffer]) {
                dependencies.push_back(out_pass);
            }
        }
        for (auto & out_as : pass->compiled_.out_acceleration_structures) {
            for (auto & in_pass : in_resource_pass_map[out_as]) {
                dependencies.push_back(in_pass);
            }
            for (auto & out_pass : out_resource_pass_map[out_as]) {
                dependencies.push_back(out_pass);
            }
        }

        // Unique the dependencies
        std::ranges::sort(dependencies);
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

        // Add edges to the graph (reverse order)
        for(auto & dep : dependencies) {
            AddEdgeRev(pass->index_, dep->index_);
        }

        // Gather resource accesses
        for(auto & in_texture : pass->compiled_.in_textures) {
            in_resource_pass_map[in_texture].emplace_back(pass.get());
        }
        for(auto & in_buffer : pass->compiled_.in_buffers) {
            in_resource_pass_map[in_buffer].emplace_back(pass.get());
        }
        for (auto & in_as : pass->compiled_.in_acceleration_structures) {
            in_resource_pass_map[in_as].emplace_back(pass.get());
        }
        for(auto & out_texture : pass->compiled_.out_textures) {
            out_resource_pass_map[out_texture].emplace_back(pass.get());
        }
        for(auto & out_buffer : pass->compiled_.out_buffers) {
            out_resource_pass_map[out_buffer].emplace_back(pass.get());
        }
        for (auto & out_as : pass->compiled_.out_acceleration_structures) {
            out_resource_pass_map[out_as].emplace_back(pass.get());
        }
    }
    {
        // BFS to mark surviving passes
        std::vector<bool> visited(passes_.size(), false);
        std::queue<int> q;
        for (auto [i, e] : std::views::enumerate(passes_)) {
            bool flag = false;
            // Never cull this pass
            if (e->GetFlags() & RDGPassFlagBits::kNeverCull) {
                flag = true;
            }
            // The pass is writing to a resource that is meant for export, or an external resource
            for (auto res : e->compiled_.out_buffers) {
                if (res->GetFlags() & RDGResourceFlagBits::kExport || res->GetFlags() & RDGResourceFlagBits::kImported) {
                    flag = true;
                }
            }
            for (auto res : e->compiled_.out_textures) {
                if (res->GetFlags() & RDGResourceFlagBits::kExport || res->GetFlags() & RDGResourceFlagBits::kImported) {
                    flag = true;
                }
            }
            // AS is not a RDG resource, so we don't check it here.
            if (flag) {
                q.push((int)i);
                visited[i] = true;
            }
        }
        while (!q.empty()) {
            int pass_index = q.front();
            q.pop();
            for (int edge_index = pass_heads_rev[pass_index]; edge_index != -1; edge_index = edges_rev[edge_index].next_edge) {
                int to = edges_rev[edge_index].dst_pass_index;
                if (!visited[to]) {
                    visited[to] = true;
                    q.push(to);
                }
            }
        }
        // Remap the passes, as well as keeping the indices in a sorted order
        std::vector<int> pass_remap(passes_.size(), -1);
        int new_index = 0;
        for (int i = 0; i < (int)passes_.size(); i++) {
            if (visited[i]) {
                pass_remap[i] = new_index;
                culled_passes.emplace_back(std::move(passes_[i]));
                new_index++;
            }
        }
        // Remap the edges
        for (auto & e : edges_rev) {
            if (visited[e.src_pass_index] && visited[e.dst_pass_index]) {
                int from = pass_remap[e.dst_pass_index];
                int to = pass_remap[e.src_pass_index];
                AddEdge(from, to);
            }
        }
    }
    // Culling completed, write to the graph
    auto graph = RenderGraphRef(new RenderGraph(graph_name));
    graph->passes_ = std::move(culled_passes);
    for (auto & pass : graph->passes_) {
        pass->graph_ = graph.Raw();
    }
    graph->edges_ = std::move(culled_edges);
    graph->num_pass_predecessors_.resize(culled_pass_heads.size(), 0);
    graph->pass_node_heads_ = std::move(culled_pass_heads);
    // Transfer allocator, let the graph keep the ownership
    graph->allocator_ = std::move(allocator_);
    // Calculate number of predecessors for each pass
    for (int i = 0; i < (int)culled_pass_heads.size(); i++) {
        for (int edge_index = culled_pass_heads[i]; edge_index != -1; edge_index = culled_edges[edge_index].next_edge) {
            int dst = culled_edges[edge_index].dst_pass_index;
            graph->num_pass_predecessors_[dst] ++;
        }
    }
#ifndef NDEBUG
    param_struct_ptr_to_data_crc.clear();
    // Check resource aliasing. Aliased resources should have been dealt with when compiling the passes.
    for (auto & pass : graph->passes_) {
        {
            std::set<void*> buffer_ptrs;
            for (auto buffer : pass->compiled_.buffers) {
                if (!buffer_ptrs.insert(buffer.buffer.Raw()).second) {
                    MI_WARN("Found shader buffer aliasing in pass '{}', buffer '{}' is assigned to multiple shader parameters and actively used.",
                        pass->GetName(), buffer.buffer->GetName());
                }
            }
            std::set<void*> texture_ptrs;
            for (auto texture : pass->compiled_.textures) {
                auto result = texture_ptrs.insert(texture.texture.Raw());
                if (!result.second) {
                    MI_WARN("Found shader texture aliasing in pass '{}', texture '{}' is assigned to multiple shader parameters and actively used.",
                        pass->GetName(), texture.texture->GetName());
                }
            }
            std::set<void*> as_ptrs;
            for (auto as : pass->compiled_.acceleration_structures) {
                if (!as_ptrs.insert(as.as).second) {
                    MI_WARN("Found shader acceleration structure aliasing in pass '{}', AS '{}' is assigned to multiple shader parameters and actively used.",
                        pass->GetName(), as.as->GetName());
                }
            }
        }

    }
#endif
    return graph;
}


MI_NAMESPACE_END