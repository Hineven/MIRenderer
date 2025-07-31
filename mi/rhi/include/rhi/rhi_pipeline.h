/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_PIPELINE_H
#define MIRENDERER_RHI_PIPELINE_H

#include <map>
#include <unordered_map>

#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_resource.h"
#include "rhi_shader.h"
#include "rhi_desc.h"

MI_NAMESPACE_BEGIN

struct RHIPipelineResourceSlot {
    // The type of the resource
    RHIPipelineResourceType resource_type;
    // Combined access flags for this resource
    RHIGPUAccessFlags access_flags;
    // In which shaders the resource is used
    RHIShaderFrequencyFlags used_shaders;
    // The index of the resource within the list of the same typed ones
    // Used to specify the slot when binding resources to the slot.
    int slot_index;
};

class RHIPipeline : public RHIResource {
public:
    FORCEINLINE bool IsValid() const {
        return is_valid_;
    }

    // Get the resource slot by the name of the resource reflected from shaders
    RHIPipelineResourceSlot ReflectResourceSlot (std::string_view name) const;
    RHIPipelineResourceSlot ReflectResourceSlot (uint32_t name_crc) const;

    bool HasResourceSlot (std::string_view name) const;
    bool HasResourceSlot (uint32_t name_crc) const;

    // If the pipeline has access to bindless resources, this function will return true.
    FORCEINLINE bool HasBindlessResources() const {return has_bindless_resources_;}

    inline virtual ~RHIPipeline() {Reset();}

    virtual void Reset () ;

    FORCEINLINE RHIPipelineType GetType() const {
        return type_;
    }

protected:
    FORCEINLINE RHIPipeline(RHIPipelineType type) : type_(type) {}

    // Append reflected shader resources to the pipeline resources and check compatibility
    bool CheckAndRemapShaderResources (RHIShader * shader);
    // Check if all resource names are unique among different types
    bool CheckNoOverlappingNamesAmongDifferentTypes () ;
    void CheckAndSetHasBindlessResources () ;

    // Set up pipeline_resource_index_ from resource lists
    void BuildPipelineResourceIndex ();

    // We're calling this function inside the base class destructor, so
    // it can not be pure virtual.
    virtual void ResetRHI () {};

    bool is_valid_ {false};

    RHIPipelineType type_ {RHIPipelineType::kMax};
    // Aggregated by the pipeline
    USE_PIPELINE_REFLECTION_STRUCTS

    std::unordered_map<uint32_t, RHIPipelineResourceSlot> pipeline_resource_index_;

    // Reflected shader resources from spv shader bytecode
    std::vector<UniformBufferDesc> uniform_buffers_;
    std::vector<StorageBufferDesc> storage_buffers_;
    std::vector<UAVDesc> uavs_;
    std::vector<SRVDesc> srvs_;
    std::vector<SamplerDesc> samplers_;
    std::vector<ImmutableSamplerDesc> immutable_samplers_;
    std::vector<AccelerationStructureDesc> acceleration_structures_;
    std::vector<CommandConstantDesc> command_constant_;


    // This should be set by the derived class implementation upon compilation
    bool has_bindless_resources_ {false};

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

    virtual void Reset () override;
    void Compile (const RHIGraphicsPipelineDesc &) ;

    FORCEINLINE const std::vector<ShaderVertexInputDesc> & GetVertexInputDesc() const {
        return vertex_inputs_;
    }
    FORCEINLINE const std::vector<ShaderFragmentOutputDesc> & GetFragmentOutputDesc() const {
        return fragment_outputs_;
    }
    FORCEINLINE bool IsDepthTestEnabled() const {
        return depth_test_enable_;
    }

    virtual ~RHIGraphicsPipeline() = default;
protected:

    FORCEINLINE RHIGraphicsPipeline (): RHIPipeline(RHIPipelineType::kGraphics) {}

    virtual bool CompileRHI (const RHIGraphicsPipelineDesc &) = 0;

    bool depth_test_enable_ {false};

    std::vector<ShaderVertexInputDesc> vertex_inputs_;
    std::vector<ShaderFragmentOutputDesc> fragment_outputs_;
};

class RHIComputePipeline : public RHIPipeline {
public:
    void Compile (RHIShader * compute_shader) ;
protected:
    FORCEINLINE RHIComputePipeline() : RHIPipeline(RHIPipelineType::kCompute) {}
    virtual ~RHIComputePipeline() = default;
    virtual bool CompileRHI (RHIShader * compute_shader) = 0;
};

class RHIRayTracingPipeline : public RHIPipeline {
public:
    void Compile(const RHIRayTracingPipelineDesc& desc);

    // Get shader group count
    uint32_t GetShaderGroupCount() const { return shader_group_count_; }

    // Get shader group handle size (for SBT construction)
    virtual uint32_t GetShaderGroupHandleSize() const = 0;

    // Get shader group handles (for SBT construction)
    virtual bool GetShaderGroupHandles(uint32_t first_group, uint32_t group_count, void* data) const = 0;

    // Get shader group handle alignment
    virtual uint32_t GetShaderGroupHandleAlignment() const = 0;

    // Get shader group base alignment (for SBT)
    virtual uint32_t GetShaderGroupBaseAlignment() const = 0;

    // Get SBT strides for different shader types
    // These methods return the stride (in bytes) for each SBT type
    virtual uint32_t GetRaygenSBTStride() const = 0;
    virtual uint32_t GetMissSBTStride() const = 0;
    virtual uint32_t GetHitSBTStride() const = 0;
    virtual uint32_t GetCallableSBTStride() const = 0;

    // Get shader group information by type (helper methods)
    uint32_t GetRaygenGroupCount() const { return raygen_group_count_; }
    uint32_t GetMissGroupCount() const { return miss_group_count_; }
    uint32_t GetHitGroupCount() const { return hit_group_count_; }
    uint32_t GetCallableGroupCount() const { return callable_group_count_; }

    void Reset() override;

protected:
    FORCEINLINE RHIRayTracingPipeline() : RHIPipeline(RHIPipelineType::kRayTracing) {}
    virtual ~RHIRayTracingPipeline() = default;
    virtual bool CompileRHI(const RHIRayTracingPipelineDesc& desc) = 0;

    uint32_t shader_group_count_ = 0;
    uint32_t max_recursion_depth_ = 1;

    // Shader group counts by type (calculated during compilation)
    uint32_t raygen_group_count_ = 0;
    uint32_t miss_group_count_ = 0;
    uint32_t hit_group_count_ = 0;
    uint32_t callable_group_count_ = 0;
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_PIPELINE_H
