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
#include <vulkan-memory-allocator-hpp/vk_mem_alloc.hpp>
#include "rhi/rhi.h"


// Minimum Vulkan API version required by the RHI implementation to work
#define MI_MIN_VULKAN_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, 201)

MI_NAMESPACE_BEGIN

class VulkanBindlessManager;
class VulkanCommandExecutor;

class VulkanRHI : public RHI {
protected:
    void PostInitialize() override;
public:
    VulkanRHI() ;
    ~VulkanRHI() override ;

    // Virtual functions from RHI interface
    inline RHIType GetType() const override {
        return RHIType::kVulkan;
    }
    inline const char * GetName () const override {
        return "Vulkan";
    }

    RHIBufferRef CreateBuffer(size_t size, RHIBufferUsageFlagBits type) override;

    RHITextureRef CreateTexture(RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
                                RHITextureUsageFlags usage, int mip_levels, int array_layers) override;

    RHISamplerRef CreateSampler(RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) override;

    RHIShaderRef CreateShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                              RHIShaderIRType ir_type, std::span<const std::byte> ir) override;

    RHIGraphicsPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc &desc, const char * name) override;

    RHIComputePipelineRef CreateComputePipeline(RHIShader *shader, const char * name) override;

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

    FORCEINLINE vma::Allocator GetVmaAllocator () const {
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

    uint32_t GetGraphicsQueueFamilyIndex();

    uint32_t GetQueueFamilyIndex(RHICommandQueueType type);

    FORCEINLINE VulkanBindlessManager * GetVulkanBindlessManager() {
        return (VulkanBindlessManager*)bindless_manager_;
    }

protected:

    void FreeResource_RHIThread(RHIResource * resource) override;

    void InvalidateDiskPipelineCache () ;
    void LoadPipelineCache ();

    VulkanCommandExecutor * command_executor_ {};

    vk::Instance instance_ {};
    vk::PhysicalDevice physical_device_ {};
    vk::Device device_ {};
    vk::Queue queue_ {};
    vk::SurfaceKHR surface_ {};
    vk::SwapchainKHR swapchain_ {};
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

    uint32_t graphics_queue_family_index_ {};
};

// Shortcut to get the Vulkan RHI instance, only valid when the RHI is initialized to Vulkan
VulkanRHI * GetVulkanRHI () ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_RHI_H
