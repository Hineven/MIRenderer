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
    RHIPipelineResourceSlot ReflectResourceSlot (const std::string & name) const;

    FORCEINLINE bool HasBindlessResources() const {return has_bindless_resources_;}

    FORCEINLINE const std::string & GetName () const {return name_;}
    FORCEINLINE void SetName () {name_ = name_; OnNameChanged();}

    RHIPipeline(std::string_view name) : name_(name) {}
    inline virtual ~RHIPipeline() {Reset();}

    void Reset () ;

    // Return the size of a adequate btb buffer for this pipeline, in bytes.
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

    virtual void ResetRHI () = 0;
    virtual void OnNameChanged () = 0;

    std::string name_;
    bool is_valid_ {false};

    // Aggregated by the pipeline
    USE_PIPELINE_REFLECTION_STRUCTS

    std::vector<UniformBufferDesc> uniform_buffers_;
    std::vector<StorageBufferDesc> storage_buffers_;
    std::vector<UAVDesc> uavs_;
    std::vector<SRVDesc> srvs_;
    std::vector<SamplerDesc> samplers_;
    std::vector<ImmutableSamplerDesc> immutable_samplers_;
    std::vector<AccelerationStructureDesc> acceleration_structures_;
    std::vector<CommandConstantDesc> command_constant_;

    bool has_bindless_resources_ {false};
    uint32_t bindless_table_size_ {};

public:
    FORCEINLINE const std::vector<UniformBufferDesc> & GetUniformBufferDesc() const {
        return uniform_buffers_;
    }
    FORCEINLINE const std::vector<StorageBufferDesc> & GetStorageBufferDesc() const {
        return storage_buffers_;
    }
    FORCEINLINE const std::vector<UAVDesc> & GetUAVDesc() const {
        return uavs_;
    }
    FORCEINLINE const std::vector<SRVDesc> & GetSRVDesc() const {
        return srvs_;
    }
    FORCEINLINE const std::vector<SamplerDesc> & GetSamplerDesc() const {
        return samplers_;
    }
    FORCEINLINE const std::vector<ImmutableSamplerDesc> & GetImmutableSamplerDesc() const {
        return immutable_samplers_;
    }
    FORCEINLINE const std::vector<AccelerationStructureDesc> & GetAccelerationStructureDesc() const {
        return acceleration_structures_;
    }
    FORCEINLINE const CommandConstantDesc & GetCommandConstantDesc() const {
        return command_constant_[0];
    }
};

class RHIGraphicsPipeline : public RHIPipeline {
public:
    using RHIPipeline::RHIPipeline;
    void Compile (const RHIGraphicsPipelineDesc &) ;

    virtual ~RHIGraphicsPipeline() = default;
protected:
    virtual bool CompileRHI (const RHIGraphicsPipelineDesc &) = 0;
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
