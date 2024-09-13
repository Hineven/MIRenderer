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
#include <vulkan-memory-allocator-hpp/vk_mem_alloc.hpp>
#include "rhi/rhi_pipeline.h"

// Minimum Vulkan API version required by the RHI implementation to work
#define MI_MIN_VULKAN_API_VERSION VK_MAKE_API_VERSION(0, 1, 3, 201)

MI_NAMESPACE_BEGIN

class VulkanRHI : public RHI {
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

    RHIBufferRef CreateBuffer(size_t size, RHIBufferUsageFlagBits type, RHIGPUAccessFlagBits access_type) override;

    RHITextureRef CreateTexture(RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
                                RHITextureUsageFlags usage, int mip_levels, int array_layers) override;

    RHISamplerRef CreateSampler(RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) override;

    RHIShaderRef CreateShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                              RHIShaderIRType ir_type, std::span<const std::byte> ir) override;

    RHIGraphicsPipelineRef CreateGraphicsPipeline(const RHIGraphicsPipelineDesc &desc) override;

    RHIComputePipelineRef CreateComputePipeline(RHIShader *shader) override;

    void ResetPipelineCache() override;

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

    RHIBindlessSupportInfo QueryRHIBindlessSupportInfo() override;


protected:

    void InvalidateDiskPipelineCache () ;
    void LoadPipelineCache ();

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
