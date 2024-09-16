/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_SHADER_H
#define MI_RHI_SHADER_H

#include <vector>
#include <memory>
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
    FORCEINLINE const std::string & GetEntryName () const { return entry_name_; }

    USE_SHADER_REFLECTION_STRUCTS

    FORCEINLINE RHIShaderFrequencyFlagBits GetFrequency() const { return frequency_; }
    FORCEINLINE bool HasBindlessResources() const { return has_bindless_resources_; }

    // The returned vector contains information about the bindless table,
    // which is different from pipelines that striped the bindless table from uniform descriptions.
    FORCEINLINE const std::vector<UniformBufferDesc> & GetUniformBufferDesc() const { return uniform_buffers_with_bindless_table_; }

    FORCEINLINE const std::vector<StorageBufferDesc> & GetStorageBufferDesc() const { return storage_buffers_; }
    FORCEINLINE const std::vector<UAVDesc> & GetUAVDesc() const { return uavs_; }
    FORCEINLINE const std::vector<SRVDesc> & GetSRVDesc() const { return srvs_; }
    FORCEINLINE const std::vector<SamplerDesc> & GetSamplerDesc() const { return samplers_; }
    FORCEINLINE const std::vector<ImmutableSamplerDesc> & GetImmutableSamplerDesc() const { return immutable_samplers_; }
    FORCEINLINE const std::vector<AccelerationStructureDesc> & GetAccelerationStructureDesc() const { return acceleration_structures_; }
    FORCEINLINE const CommandConstantDesc & GetCommandConstantDesc() const { return command_constant_[0]; }
    FORCEINLINE const std::vector<ShaderVertexInputDesc> & GetVertexInputDesc() const { return vertex_inputs_; }

protected:

    virtual bool CompileRHI () = 0;
    virtual void ResetRHI () = 0;

    bool ReflectShaderResources ();
    bool ReflectShaderResourcesSPIRV ();

    RHIShaderFrequencyFlagBits frequency_;

    std::string entry_name_;

    std::unique_ptr<std::byte[]> ir_;
    uint32_t ir_size_;
    RHIShaderIRType ir_type_;

    std::vector<UniformBufferDesc> uniform_buffers_with_bindless_table_;
    std::vector<StorageBufferDesc> storage_buffers_;
    std::vector<UAVDesc> uavs_;
    std::vector<SRVDesc> srvs_;
    std::vector<SamplerDesc> samplers_;
    std::vector<ImmutableSamplerDesc> immutable_samplers_;
    std::vector<AccelerationStructureDesc> acceleration_structures_;
    // The vector should be of length 1.
    std::vector<CommandConstantDesc> command_constant_;

    // Only makes sense for vertex shaders
    std::vector<ShaderVertexInputDesc> vertex_inputs_;

    bool has_bindless_resources_ {};
    int  bindless_table_uniform_index_ {};

    bool is_valid_ {};
};



MI_NAMESPACE_END

#endif //MI_RHI_SHADER_H