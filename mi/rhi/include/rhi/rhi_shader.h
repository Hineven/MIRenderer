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

    FORCEINLINE void SetSourceFilePath (std::string_view path) { source_file_path_ = path; }
    FORCEINLINE std::string GetSourceFilePath () const { return source_file_path_; }

    USE_SHADER_REFLECTION_STRUCTS

    FORCEINLINE RHIShaderFrequencyFlagBits GetFrequency() const { return frequency_; }
    FORCEINLINE bool HasBindlessResources() const { return has_bindless_resources_; }

    // Find a corresponding resource index among its kind in the shader. -1 if not found.
    int ReflectResourceIndex (RHIPipelineResourceType type, uint32_t name_crc) const ;

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
    FORCEINLINE bool  HasCommandConstant() const { return !command_constant_.empty(); }
    FORCEINLINE const std::vector<ShaderVertexInputDesc> & GetVertexInputDesc() const { return vertex_inputs_; }
    FORCEINLINE const std::vector<ShaderFragmentOutputDesc> & GetFragmentOutputDesc() const { return fragment_outputs_; }

    FORCEINLINE std::vector<std::byte> DuplicateShaderIRByteCode () {
        return std::vector<std::byte>(ir_, ir_ + ir_size_);
    }

    // Return a default vertex input attribute description for the pipeline.
    // Assume all attributes are piled up in a single buffer (which is a common case).
    std::vector<RHIVertexInputAttributeDesc> GetVertexInputAttributeDescForPipeline (int src_binding) const ;
    // Get the total size of one vertex in bytes (usually as the default stride).
    uint32_t GetVertexStride () const ;

protected:

    // Only used for debugging and tracking shaders.
    std::string source_file_path_ {"<unknown>"};

    virtual bool CompileRHI () = 0;

    // We're calling this function from the base class destructor,
    // so it can't be pure virtual function.
    virtual void ResetRHI () {}

    bool ReflectShaderResources ();
    bool ReflectShaderResourcesSPIRV ();

    RHIShaderFrequencyFlagBits frequency_;

    std::string entry_name_;

    std::byte * ir_;
    uint32_t ir_size_; // Byte size of the bytecode
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
    // Only makes sense for fragment shaders
    std::vector<ShaderFragmentOutputDesc> fragment_outputs_;

    bool has_bindless_resources_ {};
    int  bindless_table_uniform_index_ {};

    bool is_valid_ {};
};



MI_NAMESPACE_END

#endif //MI_RHI_SHADER_H