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
#include <queue>

#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_types.h"
#include "core/pixel_format.h"
#include "core/util/lockfree.h"
#include "rhi_cmd.h"

MI_NAMESPACE_BEGIN

// RHI (Render Hardware Interface) Layer and propietaries (RHI Resources)
class RHI {
protected:
    virtual ~RHI();
    // Called when GDynamicRHI is set but InitializeSingleton has not yet returned.
    virtual void PostInitialize () = 0;
public:
    // Initialize the RHI layer
    static void InitializeSingleton () ;
    // Destroy the RHI layer
    static void DestroySingleton () ;
    static bool HasSingleton () ;

    // Get the active RHI singleton
    static RHI & Get() ;

    virtual RHIType GetType() const = 0 ;

    virtual const char * GetName() const = 0;

    // Create a buffer
    virtual RHIBufferRef CreateBuffer (size_t size, RHIBufferUsageFlagBits type) = 0;

    // Create a texture
    virtual RHITextureRef CreateTexture (
        RHITextureType type,
        RHITextureDimensions dimensions,
        PixelFormatType format,
        RHITextureUsageFlags usage,
        int mip_levels = 1, int array_layers = 1
    ) = 0;

    // Import a texture from a native handle, thread safe
    // The import_desc is a pointer to the corresponding structs in `rhi_import.h`
    virtual RHITextureRef ImportTexture (
        const void * import_desc,
        RHITextureType type,
        RHITextureDimensions dimensions,
        PixelFormatType format,
        RHITextureUsageFlags usage,
        int mip_levels = 1, int array_layers = 1
    ) = 0;

    // Create a sampler, thread safe
    virtual RHISamplerRef CreateSampler (RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) = 0;

    // Create a shader, thread safe
    virtual RHIShaderRef CreateShader (RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                       RHIShaderIRType ir_type, std::span<const std::byte> ir) = 0;

    virtual RHIGraphicsPipelineRef CreateGraphicsPipeline (const RHIGraphicsPipelineDesc & desc) = 0;
    virtual RHIComputePipelineRef CreateComputePipeline (RHIShader * shader) = 0;

    virtual void ResetPipelineCache () = 0;

    virtual RHIBindlessSupportInfo QueryRHIBindlessSupportInfo () = 0;

    // Return the command executor for current RHI
    // It's just a wrapper of command translation programs from the unified RHI command representation
    // to the actual RHI backend
    virtual RHICommandExecutorInterface * GetCommandExecutor () = 0;

    // Create a sync point that can be waited on to synchronize device and host.
    virtual RHISyncPointRef CreateSyncPoint () = 0;

    // Wait for the underlying render hardware and RHI layer to finish all the commands
    // If host_only is true, only the operations pending on the host side will be waited.
    // Otherwise, all the operations including device (render hardware) queues will be waited.
    virtual void WaitForIdle (bool host_only = false) = 0;

    // Move to next frame. Performing logic like RHI resource recycling, queue flushing,
    // queue allocator swapping, etc. This should be called upon frame end.
    // @param in_sync_point a sync point that can be waited on for the device to complete executing submitted frame commands.
    void AdvanceFrame (RHISyncPoint * in_sync_point = nullptr) ;

    // The frame index of the entire RHI system
    // it is never decreased, and is increased by 1 every time AdvanceFrame is called.
    FORCEINLINE size_t GetFrameIndex () const {
        return frame_index_;
    }

    FORCEINLINE RHICommandQueueGraphics & GetGraphicsCommandQueue () {
        return graphics_command_queue_;
    }

    friend class RHIResource;
protected:

    RHI() ;

    struct RHIResourceToRecycle {
        RHIResource *resource;
        size_t frame_index;
    };

    // Resources pending for deletion (ref counters are 0)
    std::queue<RHIResourceToRecycle> resources_pending_for_deletion_ {};

    FORCEINLINE void AddResourcePendingForDeletion (RHIResource * resource) {
        resources_pending_for_deletion_.push({resource, frame_index_});
    }

    // @param force if true, all pending resources will be destroyed even if they are
    // potentially not ready to be destroyed.
    void FlushRHIResourcesPendingForDeletion(bool force = false) ;

    RHICommandQueueGraphics graphics_command_queue_ {};

    size_t frame_index_ {0};
};

// Check if the current thread is the RHI thread
bool IsRHIThread () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_H
