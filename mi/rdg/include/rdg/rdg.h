/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_H
#define RDG_H

#include <map>
#include "rdg_base.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN
class RDGShader;
class RDGPass;

class RenderGraph : public RefCounted<> {
public:
    void Execute (RDGResourcePool * pool) ;
    friend class RenderGraphBuilder;
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

    // Keep track of previous reosurce accesses, used to place barriers.
    struct ResourceAccess {
        RHIPipelineStageFlags stages {RHIPipelineStageFlagBits::kNone};
        RHIGPUAccessFlags access {RHIGPUAccessFlagBits::kNone};
    };
    std::map<RHIResource*, ResourceAccess> resource_accesses_;
};

typedef TRef<RenderGraph> RenderGraphRef;

MI_NAMESPACE_END

#endif //RDG_H
