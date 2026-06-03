/*
 * Created: 2026/5/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PASS_PARAM_TABLE_H
#define RDG_PASS_PARAM_TABLE_H

#include <map>
#include "rdg_resource.h"
#include "rdg/rdg_fwd.h"
#include "rhi/rhi_types.h"

MI_NAMESPACE_BEGIN

struct RDGShaderParamStructAndSizeInfo;
class RHIPipelineRootSignature;

// A parameter table shared by multiple RDGPass instances that use the same
// (root_signature, params_ptr) combination. It holds the globally-merged texture
// layouts and the assigned RHI parameter table id.
class RDGPassParameterTable {
public:
    uint32_t table_id_ = UINT32_MAX;
    const void* params_ptr_ = nullptr;
    RHIPipelineRootSignature* root_signature_ = nullptr;
    const RDGShaderParamStructAndSizeInfo* info_ = nullptr;

    struct TextureSlotKey {
        RHIPipelineResourceType type;
        uint32_t slot;
        bool operator<(const TextureSlotKey& o) const {
            return type < o.type || (type == o.type && slot < o.slot);
        }
    };

    // Merged texture layouts per binding slot across all passes sharing this table.
    // Keyed by (resource_type, slot_index) so that different bindings aliasing the
    // same RDGTexture can have independent layouts in the descriptor set.
    std::map<TextureSlotKey, RHITextureLayoutType> merged_layouts_;

    RHITextureLayoutType GetTextureLayout(RHIPipelineResourceType type, uint32_t slot) const {
        auto it = merged_layouts_.find({type, slot});
        if (it != merged_layouts_.end()) return it->second;
        return RHITextureLayoutType::kUndefined;
    }
};

MI_NAMESPACE_END

#endif //RDG_PASS_PARAM_TABLE_H
