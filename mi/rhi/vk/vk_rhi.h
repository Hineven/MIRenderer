/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_VK_RHI_H
#define MIRENDERERDEV_VK_RHI_H

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
// Use dynamic vulkan dispatch loader by default
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#else
#error "Include this header at the beginning of any vulkan related source code!"
        "Do not include <vulkan/vulkan.hpp> before this header!"
#endif

#include <vulkan/vulkan.hpp>
#include "vma_overrides.h"
#include <vk_mem_alloc.hpp>
#include "rhi/rhi.h"
#include "rhi/vk/vk_export.h"


// Minimum Vulkan API version required by the RHI implementation to work
#define MI_MIN_VULKAN_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, 201)

MI_NAMESPACE_BEGIN

class VulkanBindlessManager;
class VulkanCommandExecutor;

class VulkanRHI : public RHI {
protected:
    void PostInitialize() override;
public:
    // TODO remove this friend declaration.
    friend class VulkanCommandExecutor;

    VulkanRHI(const VulkanRHICreateInfo * extra) ;
    ~VulkanRHI() override ;

    // Virtual functions from RHI interface
    inline RHIType GetType() const override {
        return RHIType::kVulkan;
    }
    inline const char * GetName () const override {
        return "Vulkan";
    }

    RHITexture * GetBackBufferForFrameIndex(size_t index) const override;

    RHIBufferRef CreateBuffer(RHIBufferDesc desc) override;

    RHITextureRef CreateTexture(RHITextureDesc desc) override;

    TRef<RHIAccelerationStructure> CreateAccelerationStructure(RHIAccelerationStructureType type) override;

    RHISamplerRef CreateSampler(RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) override;

    RHIShaderRef CreateShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                              RHIShaderIRType ir_type, std::span<const std::byte> ir) override;

    RHIGraphicsPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc &desc, const char * name) override;

    RHIComputePipelineRef CreateComputePipeline(RHIShader *shader, const char * name) override;

    RHIRayTracingPipelineRef CreateRayTracingPipeline(const RHIRayTracingPipelineDesc &desc, const char *name) override;

    RHISyncPointRef CreateSyncPoint() override;

    RHICommandExecutorInterface * GetCommandExecutor() override;

    void ResetPipelineCache() override;

    void WaitForIdle (bool host_only = false) override;

    RHITextureRef ImportTexture (
        const void * import_desc,
        RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
        RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1
    ) override ;



    FORCEINLINE vk::Device GetDevice () const {
        return device_;
    }

    FORCEINLINE const vma::Allocator & GetVmaAllocator () const {
        return vma_;
    }

    FORCEINLINE vk::Queue GetGraphicsQueue () const {
        return queue_;
    }

    FORCEINLINE vk::PipelineCache GetPipelineCache () const {
        return pipeline_cache_;
    }

    FORCEINLINE vk::Queue GetQueue (RHICommandQueueType type) {
        switch (type) {
            case RHICommandQueueType::kGraphics:
                return queue_;
            default:
                return {};
        }
    }

    RHIBindlessSupportInfo QueryRHIBindlessSupportInfo() override;

    const void *GetUnderlyingGraphicsAPIHandles() const override;

    uint32_t GetGraphicsQueueFamilyIndex();

    uint32_t GetQueueFamilyIndex(RHICommandQueueType type);

    FORCEINLINE VulkanBindlessManager * GetVulkanBindlessManager() {
        return (VulkanBindlessManager*)bindless_manager_;
    }

    FORCEINLINE vk::SwapchainKHR GetSwapChain () const {return swapchain_;}

    RHIDeviceProperties GetDeviceProperties() const override;

    FORCEINLINE std::mutex & GetPipelineCacheMutex() {
        return pipeline_cache_mutex_;
    }

protected:

    bool InitializeSwapChain_RHI(const void *surface_handle_ptr, uint32_t width, uint32_t height, uint32_t * out_swapchain_size) override;

    void InvalidateDiskPipelineCache () ;
    void LoadPipelineCache ();

    VulkanCommandExecutor * command_executor_ {};

    VulkanRHIHandles export_handles_ {};

    vk::Instance instance_ {};
    vk::PhysicalDevice physical_device_ {};
    vk::Device device_ {};
    vk::Queue queue_ {};

    vk::SurfaceKHR surface_ {};
    vk::SwapchainKHR swapchain_ {};
    std::vector<vk::Image> swapchain_images;
    // Back buffer is double buffered. Then copied to the swapchain
    RHITextureRef rhi_backbuffer_textures[2];

    std::vector<vk::Semaphore> vk_swapchain_image_available_semaphores_;
    std::vector<vk::Semaphore> vk_swapchain_render_finished_semaphores_;

    std::mutex pipeline_cache_mutex_ {};
    vk::PipelineCache pipeline_cache_ {};

    int surface_offset_x_ {};
    int surface_offset_y_ {};
    uint32_t surface_size_x_ {};
    uint32_t surface_size_y_ {};

    vma::Allocator vma_ {};

    struct {
        vk::PhysicalDeviceProperties2 self {};
        vk::PhysicalDeviceDescriptorBufferPropertiesEXT descriptor_buffer {};
    } physical_device_properties_;

    RHIDeviceProperties rhi_device_properties_ {};

    uint32_t graphics_queue_family_index_ {};
};

// Shortcut to get the Vulkan RHI instance, only valid when the RHI is initialized to Vulkan
VulkanRHI * GetVulkanRHI () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_RHI_H
