/*
 * Created: 2025/2/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef D3D12_RHI_H
#define D3D12_RHI_H

#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

class D3D12RHI : public RHI
{
protected:
    D3D12RHI();
    ~D3D12RHI();
    friend class RHI;
public:
    RHIType GetType () const override ;
    const char * GetName () const override ;
    RHIBufferRef CreateBuffer (size_t size, RHIBufferUsageFlagBits type) override ;
    RHITextureRef CreateTexture (
        RHITextureType type,
        RHITextureDimensions dimensions,
        PixelFormatType format,
        RHITextureUsageFlags usage,
        int mip_levels = 1, int array_layers = 1
    ) override ;
    RHITextureRef ImportTexture (
        const void * import_desc,
        RHITextureType type,
        RHITextureDimensions dimensions,
        PixelFormatType format,
        RHITextureUsageFlags usage,
        int mip_levels = 1, int array_layers = 1
    ) override ;

    // Create a sampler, thread safe
    RHISamplerRef CreateSampler (RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) override ;

    // Create a shader, thread safe
    RHIShaderRef CreateShader (RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                       RHIShaderIRType ir_type, std::span<const std::byte> ir) override ;

    RHIGraphicsPipelineRef CreateGraphicsPipeline (const RHIGraphicsPipelineDesc & desc) override ;
    RHIComputePipelineRef CreateComputePipeline (RHIShader * shader) override ;

    void ResetPipelineCache () override ;

    RHIBindlessSupportInfo QueryRHIBindlessSupportInfo () override ;

    // Return the command executor for current RHI
    // It's just a wrapper of command translation programs from the unified RHI command representation
    // to the actual RHI backend
    RHICommandExecutorInterface * GetCommandExecutor () override ;

    // Create a sync point that can be waited on to synchronize device and host.
    RHISyncPointRef CreateSyncPoint () override ;

    // Wait for the underlying render hardware and RHI layer to finish all the commands
    // If host_only is true, only the operations pending on the host side will be waited.
    // Otherwise, all the operations including device (render hardware) queues will be waited.
    void WaitForIdle (bool host_only = false) override ;

	void PostInitialize () override;
};

MI_NAMESPACE_END

#endif //D3D12_RHI_H
