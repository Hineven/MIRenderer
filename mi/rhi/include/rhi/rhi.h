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
#include "core/util/lockfree.h"
#include "rhi_cmd.h"

MI_NAMESPACE_BEGIN

// Interface for the render hardware
class RHI : public NonMovable, public NonCopyable {
protected:
    virtual ~RHI();
    // Called when GDynamicRHI is set but InitializeSingleton has not yet returned.
    virtual void PostInitialize () = 0;
public:
    // Initialize the RHI layer
    static void InitializeSingleton (RHIType type) ;
    // Destroy the RHI layer
    static void DestroySingleton () ;
    static bool HasSingleton () ;

    // Get the active RHI singleton
    static RHI & Get() ;

    virtual RHIType GetType() const = 0 ;

    virtual const char * GetName() const = 0;

    // Create a buffer, thread safe
    FORCEINLINE RHIBufferRef CreateBuffer (size_t size, RHIBufferUsageFlags type) {
        return CreateBuffer({size, type});
    }

    virtual RHIBufferRef CreateBuffer (RHIBufferDesc desc) = 0;

    // Create a texture, thread safe
    RHITextureRef CreateTexture (RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage,
        uint32_t mip_levels = 1, uint32_t array_layers = 1) ;

    virtual RHITextureRef CreateTexture (RHITextureDesc desc) = 0;

    // Import a texture from a native handle, thread safe
    // The import_desc is a pointer to the corresponding structs in `rhi_import.h`
    virtual RHITextureRef ImportTexture (
            const void * import_desc,
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1
    ) = 0;

    // Create a sampler, thread safe
    virtual RHISamplerRef CreateSampler (RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) = 0;

    // Create a shader, thread safe
    virtual RHIShaderRef CreateShader (RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                       RHIShaderIRType ir_type, std::span<const std::byte> ir) = 0;

    virtual RHIGraphicsPipelineRef CreateGraphicsPipeline (const RHIGraphicsPipelineDesc & desc, const char * name = "unnamed") = 0;
    virtual RHIComputePipelineRef CreateComputePipeline (RHIShader * shader, const char * name = "unnamed") = 0;

    virtual void ResetPipelineCache () = 0;

    virtual RHIBindlessSupportInfo QueryRHIBindlessSupportInfo () = 0;

    // Return the command executor for current RHI
    // It's just a wrapper of command translation programs from the unified RHI command representation
    // to the actual RHI backend
    virtual RHICommandExecutorInterface * GetCommandExecutor () = 0;

    // Create a sync point that can be waited on to synchronize device and host.
    virtual RHISyncPointRef CreateSyncPoint () = 0;

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

    // Only the render thread is allowed to operate on RHI resource references
    // so there are only one producer and one consumer (RHI thread) for this queue.
    TLockFreeQueue<RHIResourceToRecycle, LockFreeQueueUserType::kOne, LockFreeQueueUserType::kOne>
        resources_pending_for_deletion_ {};
    // The resource that is not ready to be deleted in the previous frame.
    RHIResourceToRecycle remaining_resource_record_pending_for_deletion_ {};

    inline bool AddResourcePendingForDeletion (RHIResource * resource) {
        return resources_pending_for_deletion_.Push({resource, frame_index_});
    }
    // Free a resource allocated by the RHI.
    virtual void FreeResource_RHIThread (RHIResource * resource) = 0;

    // @param force if true, all pending resources will be recycled even if they are
    // potentially not ready to be recycled.
    void RecycleRHIResourcesPendingForDeletion_RHIThread(bool force = false) ;

    RHICommandQueueGraphics graphics_command_queue_ {};

    // The implementation should create their own bindless manager
    // and assign it to this pointer. It should also be manually deleted.
    RHIBindlessManager * bindless_manager_ {};
    size_t frame_index_ {0};
    uint32_t __tiny_buffer_for_hacking_ [128];

    std::unique_ptr<std::thread> rhi_thread_ {};
};

// Check if the current thread is the RHI thread
bool IsRHIThread () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_H
