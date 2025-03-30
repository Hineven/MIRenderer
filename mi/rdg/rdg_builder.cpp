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

MI_NAMESPACE_BEGIN

RenderGraphBuilder::RenderGraphBuilder () {
    allocator_ = std::make_unique<TOneTimeLinearAllocator<>>();
}

RenderGraphBuilder::~RenderGraphBuilder() {}


RDGPass * RenderGraphBuilder::AddPass(
    const char *name,
    RDGPassType pass_type,
    RDGPassFlags pass_flags,
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
    ptr->shader_param_struct_info_ = shader_param_struct_info;
    ptr->shader_param_data_ = parameter_struct;
    auto pass = std::unique_ptr<RDGPass>(ptr);
    // Compile the pass, to keep references to RDG resources alive
    pass->Compile();
    // Add to the pass list
    passes_.push_back(std::move(pass));
    return ptr;
}


TRef<RenderGraph> RenderGraphBuilder::Compile() {

    std::map<RDGResource*, std::vector<RDGPass*>> in_resource_pass_map;
    std::map<RDGResource*, std::vector<RDGPass*>> out_resource_pass_map;
    std::vector<std::unique_ptr<RDGPass>> culled_passes;
    std::vector<int> culled_pass_heads, pass_heads_rev;
    culled_pass_heads.resize(passes_.size(), -1);
    pass_heads_rev.resize(passes_.size(), -1);
    std::vector<RenderGraph::Edge> culled_edges, edges_rev;
    auto AddEdge = [&](int from, int to) {
        int edge_index = (int)culled_edges.size();
        culled_edges.emplace_back(from, to, culled_pass_heads[to]);
        culled_pass_heads[from] = edge_index;
    };
    auto AddEdgeRev = [&](int from, int to) {
        int edge_index_rev = (int)edges_rev.size();
        edges_rev.emplace_back(from, to, pass_heads_rev[to]);
        pass_heads_rev[from] = edge_index_rev;
    };
    for(auto & pass : passes_) {
        // Find dependencies (to prior passes)
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
        // RW-W
        for(auto & out_texture : pass->compiled_.out_textures) {
            for(auto & out_pass : out_resource_pass_map[out_texture]) {
                dependencies.push_back(out_pass);
            }
            for(auto & in_pass : in_resource_pass_map[out_texture]) {
                dependencies.push_back(in_pass);
            }
        }
        for (auto & out_buffer : pass->compiled_.out_buffers) {
            for (auto & out_pass : out_resource_pass_map[out_buffer]) {
                dependencies.push_back(out_pass);
            }
            for (auto & in_pass : in_resource_pass_map[out_buffer]) {
                dependencies.push_back(in_pass);
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
        for(auto & out_texture : pass->compiled_.out_textures) {
            out_resource_pass_map[out_texture].emplace_back(pass.get());
        }
        for(auto & out_buffer : pass->compiled_.out_buffers) {
            out_resource_pass_map[out_buffer].emplace_back(pass.get());
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
            // The pass is writing to a resource that is meant for export
            for (auto res : e->compiled_.out_buffers) {
                if (exporting_resources_.find(res) != exporting_resources_.end()) {
                    flag = true;
                }
            }
            for (auto res : e->compiled_.out_textures) {
                if (exporting_resources_.find(res) != exporting_resources_.end()) {
                    flag = true;
                }
            }
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
        // Remap the passes
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
    auto graph = RenderGraphRef(new RenderGraph());
    graph->passes_ = std::move(culled_passes);
    for (auto & pass : graph->passes_) {
        pass->graph_ = graph.Raw();
    }
    graph->edges_ = std::move(culled_edges);
    graph->num_pass_predecessors_.resize(culled_pass_heads.size(), 0);
    graph->pass_node_heads_ = std::move(culled_pass_heads);
    // Make the graph hold references to resources for exporting
    // so that the pool won't recycle them
    for (auto e : exporting_resources_) {
        graph->exporting_resources_.emplace_back(e);
    }
    // Transfer allocator, let the graph keep the ownership
    graph->allocator_ = std::move(allocator_);
    // Calculate number of predecessors for each pass
    for (int i = 0; i < (int)culled_pass_heads.size(); i++) {
        for (int edge_index = culled_pass_heads[i]; edge_index != -1; edge_index = culled_edges[edge_index].next_edge) {
            int dst = culled_edges[edge_index].dst_pass_index;
            graph->num_pass_predecessors_[dst] ++;
        }
    }
    return graph;
}


MI_NAMESPACE_END