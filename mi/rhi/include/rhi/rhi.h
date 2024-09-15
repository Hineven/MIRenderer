/*
 * Created: 2024/7/3
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_H
#define MIRENDERERDEV_RHI_H

#include <string>
#include <memory>
#include <future>
#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_types.h"
#include "core/pixel_format.h"
#include "util/lockfree.h"

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

    // Initialize the RHI layer
    static void InitializeSingleton (RHIType type) ;
    // Destroy the RHI layer
    static void DestroySingleton () ;

    // Get the active RHI singleton
    static RHI & Get() ;

    virtual RHIType GetType() const = 0 ;

    virtual const char * GetName() const = 0;

    // Create a buffer, thread safe
    virtual RHIBufferRef CreateBuffer (size_t size, RHIBufferUsageFlagBits type, RHIGPUAccessFlagBits access_type) = 0;

    // Create a texture, thread safe
    virtual RHITextureRef CreateTexture (RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1) = 0;

    // Import a texture from a native handle, thread safe
    // The native handle is a pointer to the texture object in the backend API
    virtual RHITextureRef ImportTexture (
            void * native_handle,
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1
    ) = 0;

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

    // Return the command executor for current RHI
    // It's just a wrapper of command translation programs from the unified RHI command representation
    // to the actual RHI backend
    virtual RHICommandExecutorInterface * GetCommandExecutor () = 0;

    FORCEINLINE RHIBindlessManager & GetBindlessManager () const {
        return *bindless_manager_;
    }

    // Wait for the underlying render hardware and RHI layer to finish all the commands
    // If host_only is true, only the operations pending on the host side will be waited.
    // Otherwise, all the operations including device (render hardware) queues will be waited.
    virtual void WaitForIdle (bool host_only = false) = 0;

    // Move to next frame. Performing logic like RHI resource recycling, queue flushing,
    // queue allocator swapping, etc.
    // @return A future that will be set when all host operations are done and the next frame
    // is ready to be rendered.
    // Note: The future will not be waiting for device operations of the previous frame to
    // complete.
    virtual std::future<void> AdvanceFrame () ;

    // The frame index of the entire RHI system
    // it is never decreased, and is increased by 1 every time AdvanceFrame is called.
    inline size_t GetFrameIndex () const {
        return frame_index_;
    }

protected:

    struct RHIResourceToRecycle {
        RHIResource *resource;
        size_t frame_index;
    };

    TLockFreeQueue<RHIResourceToRecycle, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne>
    resources_pending_for_deletion_ {};
    // The resource that is not ready to be deleted in the previous frame.
    RHIResourceToRecycle remaining_resource_record_pending_for_deletion_ {};

    // Free a resource allocated by the RHI.
    virtual void FreeResource_RHIThread (RHIResource * resource) = 0;


    void RecycleRHIResourcesPendingForDeletion_RHIThread() ;

    std::unique_ptr<RHIBindlessManager> bindless_manager_ {};
    size_t frame_index_ {0};
    uint32_t __tiny_buffer_for_hacking_ [128];
};

// Check if the current thread is the RHI thread
bool IsRHIThread () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_H
