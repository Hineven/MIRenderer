/*
 * Created: 2026/4/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_ROOT_SIGNATURE_H
#define MI_RHI_ROOT_SIGNATURE_H

#include "rhi/rhi_resource.h"
#include "rhi/rhi_types.h"

MI_NAMESPACE_BEGIN

struct RHIPipelineRootSignatureDesc;

// An immutable RHI resource representing a shared pipeline layout / root signature.
// Created via RHI::CreateRootSignature() from an RHIPipelineRootSignatureDesc.
// Pipelines compiled with the same root signature share the same descriptor set layout and pipeline layout,
// enabling descriptor set reuse across pipeline binds within a frame.
// This object owns the underlying GPU resources (descriptor set layout, pipeline layout) and is
// reference-counted via TRef<>. 
class RHIPipelineRootSignature : public RHIResource {
public:
    virtual ~RHIPipelineRootSignature() = default;

    FORCEINLINE uint32_t GetPushConstantSize() const { return push_constant_size_; }
    FORCEINLINE uint32_t GetNumResources(RHIPipelineResourceType type) const {
        return num_resources_[(uint32_t)type];
    }

    struct TypeNames {
        const uint32_t * name_crcs {};
        uint32_t count {};
    };

    FORCEINLINE const TypeNames & GetTypeNames(uint32_t type_index) const {
        return type_names_[type_index];
    }

    FORCEINLINE uint32_t GetBinding(RHIPipelineResourceType type, uint32_t param_struct_index) const {
        return binding_base_[(uint32_t)type] + param_struct_index;
    }

protected:
    uint32_t binding_base_[(uint32_t)RHIPipelineResourceType::kMax] {};
    uint32_t num_resources_[(uint32_t)RHIPipelineResourceType::kMax] {};
    TypeNames type_names_[(uint32_t)RHIPipelineResourceType::kMax] {};
    uint32_t push_constant_size_ {};
};

using RHIPipelineRootSignatureRef = TRef<RHIPipelineRootSignature>;

MI_NAMESPACE_END

#endif // MI_RHI_ROOT_SIGNATURE_H
