/*
 * Created: 2024/7/3
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_H
#define MIRENDERERDEV_RHI_H

#include <memory>
#include <future>
#include <atomic>
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_root_signature.h"
#include "core/pixel_format.h"
#include "core/util/queue.h"
#include "rhi_cmd.h"

MI_NAMESPACE_BEGIN

// Get the frame index for current thread.
// If RHI thread, get the counter from RHI thread.
// Otherwise, returns rhi.GetFrameIndex()
size_t GetFrameIndexForCurrentThread();

// Interface for the render hardware
class RHI : public NonMovable, public NonCopyable {
protected:
    virtual ~RHI();
    void PreDestruction ();
    // Called when GDynamicRHI is set but InitializeSingleton has not yet returned.
    virtual void PostInitialize () ;
public:
    // Initialize the RHI layer
    static void InitializeSingleton (RHIType type, const void * extra = nullptr) ;
    // Destroy the RHI layer
    static void DestroySingleton () ;
    static bool HasSingleton () ;

    // Get the active RHI singleton
    static RHI & Get() ;

    // Initialize swap chain on window surface for real-time windowed applications
    // @param surface_handle The handle of the window surface, RHI type dependent.
    // for Vulkan, it's a VkSurfaceKHR*
    bool InitializeSwapChain (const void * surface_handle_ptr, uint32_t width, uint32_t height) ;

    FORCEINLINE bool IsSwapChainInitialized() const {
        return is_swapchain_initialized_;
    }

    // Return the back buffer of this frame to draw to (if swapchain is initialized)
    FORCEINLINE RHITexture * GetBackBuffer () const {
        return GetBackBufferForFrameIndex(frame_index_);
    }

    // Return the back buffer of this frame to draw to (if swapchain is initialized)
    virtual RHITexture * GetBackBufferForFrameIndex (size_t index) const = 0;

    virtual RHIType GetType() const = 0 ;

    virtual const char * GetName() const = 0;

    // Create a buffer, thread safe
    RHIBufferRef CreateBuffer (size_t size, RHIBufferUsageFlags type) ;

    virtual RHIBufferRef CreateBuffer (RHIBufferDesc desc) = 0;

    // Create a texture, thread safe
    RHITextureRef CreateTexture (RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage,
        uint32_t mip_levels = 1, uint32_t array_layers = 1) ;

    virtual RHITextureRef CreateTexture (RHITextureDesc desc) = 0;

    // Import a texture from a native handle, thread safe
    // The import_desc is a pointer to the corresponding structs in `rhi/<backend>/xxx.h`
    virtual RHITextureRef ImportTexture (
            const void * import_desc,
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format, RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1
    ) = 0;

    virtual TRef<RHIAccelerationStructure> CreateAccelerationStructure (RHIAccelerationStructureType type) = 0;

    // Create a Partitioned TLAS (VK_NV_partitioned_acceleration_structure).
    // The returned object has no backing buffer allocated yet — call GetBuildSizes
    // then Allocate before issuing a build command.
    virtual TRef<RHIPartitionedTLAS> CreatePartitionedTLAS () = 0;

    // Create a sampler, thread safe
    virtual RHISamplerRef CreateSampler (RHISamplerDesc desc) = 0;

    RHISamplerRef CreateSampler (RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) ;

    // Create a GPU timestamp resource for the current frame. Each resource owns a query slot in the global query pool.
    // The timestamp will stay valid until the next frame ends on the device. You should NEVER keep references to
    // timestamps that are older than the previous frame.
    // NOTE: This is fast. DO NOT CACHE TIMESTAMPS AND USE THEM ACROSS MULTIPLE FRAMES. Create them on demand instead.
    // Simply drop them after use is preferred.
    virtual RHITimestampRef CreateTimestamp () = 0;
    // Batched query for multiple timestamps. This is faster than RHITimestamp::QueryResult
    enum class RHITimestampQueryMode : uint8_t {
        // Block the CPU until query results are ready (backend may use vk::QueryResultFlagBits::eWait).
        kBlocking = 0,
        // Non-blocking query (backend should use availability bits and return UINT64_MAX for unavailable timestamps).
        kNonBlocking,
    };

    // Returns a value per timestamp. If kNonBlocking is used, any timestamp not yet available MUST be returned as UINT64_MAX.
    virtual std::vector<uint64_t> QueryTimestamps (std::span<RHITimestamp*> timestamps,
        RHITimestampQueryMode mode) = 0;

    // Convenience overload: blocking query.
    FORCEINLINE std::vector<uint64_t> QueryTimestamps(std::span<RHITimestamp*> timestamps) {
        return QueryTimestamps(timestamps, RHITimestampQueryMode::kBlocking);
    }

    // Create a shader, thread safe
    virtual RHIShaderRef CreateShader (RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                       RHIShaderIRType ir_type, std::span<const std::byte> ir) = 0;

    virtual RHIGraphicsPipelineRef CreateGraphicsPipeline (const RHIGraphicsPipelineDesc & desc, const char * name, RHIPipelineRootSignature * root) = 0;
    virtual RHIComputePipelineRef CreateComputePipeline (RHIShader * shader, const char * name, RHIPipelineRootSignature * root) = 0;
    virtual RHIRayTracingPipelineRef CreateRayTracingPipeline (
        const RHIRayTracingPipelineDesc & desc, const char * name, RHIPipelineRootSignature * root
    ) = 0;

    virtual RHIPipelineRootSignatureRef CreateRootSignature (const RHIPipelineRootSignatureDesc & desc) = 0;

    // Reset the pipeline cache if the cache size exceeds the given limit (bytes).
    virtual void ResetPipelineCache (uint32_t size_limit = 0) = 0;

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

    // Flush all queues. Wait for the underlying render hardware and RHI layer to finish all the commands
    // If host_only is true, only the operations pending on the host side will be waited.
    // Otherwise, all the operations including device (render hardware) queues will be waited.
    virtual void WaitForIdle (bool host_only = false) = 0;

    // Move to next frame. Performing logic like RHI resource recycling, queue flushing,
    // queue allocator swapping, etc.
    // This SHOULD NOT be called if the frame before the current frame (which is ending) is not
    // finished on the device yet.
    // @param sync_point A sync point that can be waited on for the device to complete executing the whole frame.
    // @return A future that will be set when all HOST operations are done and the next frame
    // is ready to be rendered.
    virtual std::future<void> AdvanceFrame (RHISyncPoint * sync_point = nullptr) ;

    // The frame index of the entire RHI system
    // it is never decreased, and is increased by 1 every time AdvanceFrame is called.
    FORCEINLINE size_t GetFrameIndex () const {
        return frame_index_;
    }

    FORCEINLINE RHICommandQueueGraphics & GetGraphicsCommandQueue () {
        return graphics_command_queue_;
    }

    // Return a pointer to a struct containing the underlying graphics API handles
    // You should cast the pointer to the corresponding struct type for the RHI backend
    virtual const void * GetUnderlyingGraphicsAPIHandles () const = 0;

    friend class RHIResource;

    struct GlobalSamplers {
        RHISampler * linear_wrap;
        RHISampler * linear_edge;
        RHISampler * point_wrap;
        RHISampler * point_edge;
        RHISampler * point_border_1;
        RHISampler * point_border_0;
    };

    FORCEINLINE GlobalSamplers GetGlobalSamplers () const {
        return global_samplers_;
    }

    virtual RHIDeviceProperties GetDeviceProperties () const = 0;

    // Get the stride of input instance header when building TLAS.
    virtual uint32_t GetAccelerationStructureInstanceStride () const = 0;
    // Create acceleration structure instance headers from given descriptions.
    virtual void CreateAccelerationStructureInstances(uint32_t count,
        const RHIAccelerationStructureInstanceDesc * in_desc, void * out_desc) const = 0;

protected:

    GlobalSamplers global_samplers_ {};

    RHI() ;

    struct RHIResourceToRecycle {
        RHIResource *resource;
        size_t frame_index;
    };

    // Only the render thread is allowed to operate on RHI resource references
    // so there are only one producer and one consumer (RHI thread) for this queue.
    TLockFreeQueue<RHIResourceToRecycle, LockFreeQueueUserType::kOne, LockFreeQueueUserType::kOne, 16384>
        resources_pending_for_deletion_ {};
    // The resource that is not ready to be deleted in the previous frame.
    RHIResourceToRecycle remaining_resource_record_pending_for_deletion_ {};
    std::atomic<size_t> resources_pending_for_deletion_count_ {0};

    // The function can be called from BOTH render thread and RHI thread. frame_index_ counter is retrieved from
    // either sides.
    FORCEINLINE bool AddResourcePendingForDeletion (RHIResource * resource) {
        bool pushed = resources_pending_for_deletion_.Push({resource, GetFrameIndexForCurrentThread()});
        if (pushed) {
            resources_pending_for_deletion_count_.fetch_add(1, std::memory_order_relaxed);
        }
        return pushed;
    }

    // @param force if true, all pending resources will be recycled even if they are
    // potentially not ready to be recycled.
    void RecycleRHIResourcesPendingForDeletion_RHIThread(bool force = false) ;


    virtual bool InitializeSwapChain_RHI (const void * surface_handle_ptr, uint32_t width, uint32_t height, uint32_t * swapchain_size) = 0;

    RHICommandQueueGraphics graphics_command_queue_ {};

    // The implementation should create their own bindless manager
    // and assign it to this pointer. It should also be manually deleted.
    RHIBindlessManager * bindless_manager_ {};
    size_t frame_index_ {0};

    std::unique_ptr<std::thread> rhi_thread_ {};

    bool is_swapchain_initialized_ {};
    uint32_t swapchain_size_ {};
};

// Check if the current thread is the RHI thread
bool IsRHIThread () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_H
