/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_PIPELINE_H
#define MIRENDERER_RHI_PIPELINE_H

#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_resource.h"
#include "rhi_shader.h"
#include "rhi_desc.h"
#include "core/conalloc.h"

MI_NAMESPACE_BEGIN

struct RHIPipelineResourceSlot {
    // The type of the resource
    RHIPipelineResourceType resource_type;
    // In which stages the resource is used
    RHIShaderFrequencyFlagBits available_stages;
    // The index of the resource within the list of the same typed ones
    int resource_index;
};

class RHIPipeline : public RHIResource {
public:
    FORCEINLINE bool IsValid() const {
        return is_valid_;
    }

    // Get the resource slot by the name of the resource reflected from shaders
    RHIPipelineResourceSlot ReflectResourceSlot (const IString & name) const;

    FORCEINLINE bool HasBindlessResources() const {return has_bindless_resources_;}

    FORCEINLINE const IString & GetName () const {return name_;}
    FORCEINLINE void SetName () {name_ = name_; OnNameChanged();}

    RHIPipeline(std::string_view name) : name_(name) {}
    inline virtual ~RHIPipeline() {Reset();}

    virtual void Reset () ;

    // Return the size of an adequate btb buffer for this pipeline, in number of slots.
    inline uint32_t GetBindlessTableSize () const {return bindless_table_size_;}

protected:
    // Append reflected shader resources to the pipeline resources and check compatibility
    bool CheckAndRemapShaderResources (RHIShader * shader);
    // Check if all resource names are unique among different types
    bool CheckNoOverlappingNamesAmongDifferentTypes () ;
    // There should be a special uniform buffer definition within the shaders supporting
    // bindless resources. The buffer is the bindless table buffer.
    // Try to locate the bindless table uniform buffer from reflected pipeline resources
    // , remove it from uniforms_ and set relating attributes.
    void TryLocateAndStripBindlessTableUniformBuffer () ;

    // We're calling this function inside the base class destructor, so
    // it can not be pure virtual.
    virtual void ResetRHI () {};
    virtual void OnNameChanged () = 0;

    IString name_;
    bool is_valid_ {false};

    // Aggregated by the pipeline
    USE_PIPELINE_REFLECTION_STRUCTS

    IVector<UniformBufferDesc> uniform_buffers_;
    IVector<StorageBufferDesc> storage_buffers_;
    IVector<UAVDesc> uavs_;
    IVector<SRVDesc> srvs_;
    IVector<SamplerDesc> samplers_;
    IVector<ImmutableSamplerDesc> immutable_samplers_;
    IVector<AccelerationStructureDesc> acceleration_structures_;
    IVector<CommandConstantDesc> command_constant_;

    bool has_bindless_resources_ {false};
    // Number of slots in the btb table
    uint32_t bindless_table_size_ {};

public:
    FORCEINLINE const IVector<UniformBufferDesc> & GetUniformBufferDesc() const {
        return uniform_buffers_;
    }
    FORCEINLINE const IVector<StorageBufferDesc> & GetStorageBufferDesc() const {
        return storage_buffers_;
    }
    FORCEINLINE const IVector<UAVDesc> & GetUAVDesc() const {
        return uavs_;
    }
    FORCEINLINE const IVector<SRVDesc> & GetSRVDesc() const {
        return srvs_;
    }
    FORCEINLINE const IVector<SamplerDesc> & GetSamplerDesc() const {
        return samplers_;
    }
    FORCEINLINE const IVector<ImmutableSamplerDesc> & GetImmutableSamplerDesc() const {
        return immutable_samplers_;
    }
    FORCEINLINE const IVector<AccelerationStructureDesc> & GetAccelerationStructureDesc() const {
        return acceleration_structures_;
    }
    FORCEINLINE const CommandConstantDesc & GetCommandConstantDesc() const {
        return command_constant_[0];
    }
};

class RHIGraphicsPipeline : public RHIPipeline {
public:
    using RHIPipeline::RHIPipeline;

    virtual void Reset () override;
    void Compile (const RHIGraphicsPipelineDesc &) ;

    FORCEINLINE const IVector<ShaderVertexInputDesc> & GetVertexInputDesc() const {
        return vertex_inputs_;
    }
    FORCEINLINE const IVector<ShaderFragmentOutputDesc> & GetFragmentOutputDesc() const {
        return fragment_outputs_;
    }
    FORCEINLINE bool IsDepthTestEnabled() const {
        return depth_test_enable_;
    }

    virtual ~RHIGraphicsPipeline() = default;
protected:
    virtual bool CompileRHI (const RHIGraphicsPipelineDesc &) = 0;

    bool depth_test_enable_ {false};

    IVector<ShaderVertexInputDesc> vertex_inputs_;
    IVector<ShaderFragmentOutputDesc> fragment_outputs_;
};

class RHIComputePipeline : public RHIPipeline {
public:
    using RHIPipeline::RHIPipeline;
    void Compile (RHIShader * compute_shader) ;
protected:
    virtual ~RHIComputePipeline() = default;
    virtual bool CompileRHI (RHIShader * compute_shader) = 0;
};
MI_NAMESPACE_END

#endif //MIRENDERER_RHI_PIPELINE_H
