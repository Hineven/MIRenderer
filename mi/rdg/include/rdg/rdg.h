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
class RDGShader;
class RDGPass;

class RenderGraph : public RefCounted<> {
public:
    ~RenderGraph();
    void Execute (RDGResourcePool * pool, RHISyncPoint * sync_point = nullptr) ;
    friend class RenderGraphBuilder;

    // Used for running passes to query underlying uniform buffers with parameter struct pointers
    FORCEINLINE RDGBuffer * GetUniformBufferForParameterStruct (const void * ptr) const {
        auto it = ref_buffer_map_.find(ptr);
        if (it == ref_buffer_map_.end()) {
            return nullptr;
        }
        return it->second.Raw();
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

    // Used to query referenced uniform buffer resources according to parameter structs when running the graph
    // Referenced buffers are not directly created via pass->uniform_buffer_, so we have to manually create them
    // The map keeps a relation between cpp param struct pointers and the corresponding RDGBuffer
    std::map<const void*, RDGBufferRef> ref_buffer_map_;

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
