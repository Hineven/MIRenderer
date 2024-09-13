/*
 * Created: 2024/7/3
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_H
#define MIRENDERERDEV_RHI_H

#include <string>
#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_types.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

// Interface for the render hardware
class RHI {
protected:
    RHI() = default;
    virtual ~RHI() = default;
    // friends within rhi_singleton.cpp
    friend void RHIInitialize (RHIType type) ;
    friend void RHIDestroy () ;
public:
    // Get the active RHI instance
    static RHI & GetInstance() ;

    virtual RHIType GetType() const = 0 ;

    virtual const char * GetName() const = 0;

    // Create a buffer, thread safe
    virtual RHIBufferRef CreateBuffer (size_t size, RHIBufferUsageFlagBits type, RHIGPUAccessFlagBits access_type) = 0;

    // Create a texture, thread safe
    virtual RHITextureRef CreateTexture (RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1) = 0;

    virtual RHITextureRef ImportTexture ()

    // Create a sampler, thread safe
    virtual RHISamplerRef CreateSampler (RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) ;

    // Create a shader, thread safe
    virtual RHIShaderRef CreateShader (RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                       RHIShaderIRType ir_type, std::span<const std::byte> ir) = 0;

    virtual RHIGraphicsPipelineRef CreateGraphicsPipeline (const RHIGraphicsPipelineDesc & desc) = 0;
    virtual RHIComputePipelineRef CreateComputePipeline (RHIShader * shader) = 0;

    virtual void ResetPipelineCache () = 0;

    virtual void SetBindlessEnable (bool enable) = 0;

    virtual RHIBindlessSupportInfo QueryRHIBindlessSupportInfo () = 0;

    virtual RHICommandExecutor * GetCommandExecutor () = 0;

    FORCEINLINE RHIBindlessManager & GetBindlessManager () const {
        return *bindless_manager_;
    }

protected:
    std::unique_ptr<RHIBindlessManager> bindless_manager_ {};
};

// Check if the current thread is the RHI thread
bool IsRHIThread () ;

// Initialize the RHI layer
void RHIInitialize (RHIType type) ;

// Destroy the RHI layer
void RHIDestroy () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_H
