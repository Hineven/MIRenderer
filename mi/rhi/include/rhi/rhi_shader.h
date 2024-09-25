/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_SHADER_H
#define MI_RHI_SHADER_H

#include <vector>
#include <memory>
#include "core/conalloc.h"
#include "rhi/rhi_common.h"
#include "rhi/rhi_resource.h"
#include "rhi_desc.h"

MI_NAMESPACE_BEGIN

class RHIShader : public RHIResource {
public:
    RHIShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
              RHIShaderIRType ir_type, std::span<const std::byte> ir) ;
    ~RHIShader() override ;

    void Compile () ;
    void Reset () ;

    FORCEINLINE bool IsValid () const { return is_valid_; }
    FORCEINLINE const IString & GetEntryName () const { return entry_name_; }

    USE_SHADER_REFLECTION_STRUCTS

    FORCEINLINE RHIShaderFrequencyFlagBits GetFrequency() const { return frequency_; }
    FORCEINLINE bool HasBindlessResources() const { return has_bindless_resources_; }

    // The returned vector contains information about the bindless table,
    // which is different from pipelines that striped the bindless table from uniform descriptions.
    FORCEINLINE const IVector<UniformBufferDesc> & GetUniformBufferDesc() const { return uniform_buffers_with_bindless_table_; }

    FORCEINLINE const IVector<StorageBufferDesc> & GetStorageBufferDesc() const { return storage_buffers_; }
    FORCEINLINE const IVector<UAVDesc> & GetUAVDesc() const { return uavs_; }
    FORCEINLINE const IVector<SRVDesc> & GetSRVDesc() const { return srvs_; }
    FORCEINLINE const IVector<SamplerDesc> & GetSamplerDesc() const { return samplers_; }
    FORCEINLINE const IVector<ImmutableSamplerDesc> & GetImmutableSamplerDesc() const { return immutable_samplers_; }
    FORCEINLINE const IVector<AccelerationStructureDesc> & GetAccelerationStructureDesc() const { return acceleration_structures_; }
    FORCEINLINE const CommandConstantDesc & GetCommandConstantDesc() const { return command_constant_[0]; }
    FORCEINLINE bool  HasCommandConstant() const { return !command_constant_.empty(); }
    FORCEINLINE const IVector<ShaderVertexInputDesc> & GetVertexInputDesc() const { return vertex_inputs_; }
    FORCEINLINE const IVector<ShaderFragmentOutputDesc> & GetFragmentOutputDesc() const { return fragment_outputs_; }

    // Return a default vertex input attribute description for the pipeline.
    // Assume all attributes are piled up in a single buffer (which is a common case).
    IVector<RHIVertexInputAttributeDesc> GetVertexInputAttributeDescForPipeline (int src_binding) const ;
    // Get the total size of one vertex in bytes (usually as the default stride).
    uint32_t GetVertexStride () const ;

protected:

    virtual bool CompileRHI () = 0;

    // We're calling this function from the base class destructor,
    // so it can't be pure virtual function.
    virtual void ResetRHI () {}

    bool ReflectShaderResources ();
    bool ReflectShaderResourcesSPIRV ();

    RHIShaderFrequencyFlagBits frequency_;

    IString entry_name_;

    std::byte * ir_;
    uint32_t ir_size_;
    RHIShaderIRType ir_type_;

    IVector<UniformBufferDesc> uniform_buffers_with_bindless_table_;
    IVector<StorageBufferDesc> storage_buffers_;
    IVector<UAVDesc> uavs_;
    IVector<SRVDesc> srvs_;
    IVector<SamplerDesc> samplers_;
    IVector<ImmutableSamplerDesc> immutable_samplers_;
    IVector<AccelerationStructureDesc> acceleration_structures_;
    // The vector should be of length 1.
    IVector<CommandConstantDesc> command_constant_;

    // Only makes sense for vertex shaders
    IVector<ShaderVertexInputDesc> vertex_inputs_;
    // Only makes sense for fragment shaders
    IVector<ShaderFragmentOutputDesc> fragment_outputs_;

    bool has_bindless_resources_ {};
    int  bindless_table_uniform_index_ {};

    bool is_valid_ {};
};



MI_NAMESPACE_END

#endif //MI_RHI_SHADER_H