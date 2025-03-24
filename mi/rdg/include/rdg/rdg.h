/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_H
#define RDG_H

#include <map>
#include "rdg_base.h"
#include "rdg_resource.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN
struct RDGShaderParamStructAndSizeInfo;
class RDGShader;
class RDGPass;

class RenderGraph : public RefCounted<> {
public:
    ~RenderGraph();
    void Execute (RDGResourcePool * pool, RHISyncPoint * sync_point = nullptr) ;
    friend class RenderGraphBuilder;

    struct RDGUniformBufferPtr {
        RDGBuffer * buffer {};
        size_t offset;
    };
    // Used for running passes to query underlying uniform buffers with parameter struct pointers
    FORCEINLINE RDGUniformBufferPtr GetUniformBufferForParameterStruct (const void * ptr) const {
        auto it = param_ptr_to_uniform_buffer_segment_.find(ptr);
        if (it == param_ptr_to_uniform_buffer_segment_.end()) {
            return {};
        }
        return {uniform_buffer_.Raw(), it->second.offset};
    }
protected:
    std::vector<std::unique_ptr<RDGPass>> passes_;
    struct Edge {
        int src_pass_index;
        int dst_pass_index;
        int next_edge;
    };
    std::vector<int>  pass_node_heads_;
    std::vector<Edge> edges_;
    std::vector<int>  num_pass_predecessors_;
    // Hold references to resources for exporting (prevent them from being evicted from the pool)
    std::vector<RDGResourceRef> exporting_resources_;

    struct UniformBufferSegment {
        size_t offset;
        const RDGShaderParamStructAndSizeInfo * param_info;
    };
    // Used to query uniform buffer offsets according to parameter structs when running the graph.
    std::map<const void*, UniformBufferSegment> param_ptr_to_uniform_buffer_segment_;
    // The uniform buffer that holds all the uniform data for all the passes.
    TRef<RDGBuffer> uniform_buffer_;

    // Keep track of previous reosurce accesses, used to place barriers.
    struct ResourceAccess {
        RHIPipelineStageFlags stages {RHIPipelineStageFlagBits::kNone};
        RHIGPUAccessFlags access {RHIGPUAccessFlagBits::kNone};
    };
    std::map<RHIResource*, ResourceAccess> resource_accesses_;

    // Temporary memory allocator (transferred from the RDG builder)
    std::unique_ptr<TOneTimeLinearAllocator<>> allocator_;
};

typedef TRef<RenderGraph> RenderGraphRef;

MI_NAMESPACE_END

#endif //RDG_H
