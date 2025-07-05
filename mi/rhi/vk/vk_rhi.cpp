/*
 * Created: 2024/7/3
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <string>
#include <fstream>
#include "rhi/rhi.h"
#include "core/infra.h"

#include "vk_rhi.h"
#include "rhi/vk/vk_export.h"

#include <glfw/glfw3.h>

#include "vk_resource.h"
#include "vk_buffer.h"
#include "vk_texture.h"
#include "vk_shader.h"
#include "vk_pipeline.h"
#include "vk_bindless.h"
#include "vk_cmd_exec.h"
#include "vk_conversion.h"
#include "rhi/rhi_thread.h"

MI_NAMESPACE_BEGIN

VulkanRHI::VulkanRHI(const VulkanRHICreateInfo * extra) {
    {
        VULKAN_HPP_DEFAULT_DISPATCHER.init();

        vk::ApplicationInfo app_info("MIRenderer",
                                     VK_MAKE_VERSION(MI_APPLICATION_VERSION_MAJOR, MI_APPLICATION_VERSION_MINOR, 0),
                                     MI_ENGINE_NAME,
                                     VK_MAKE_VERSION(MI_ENGINE_VERSION_MAJOR, MI_ENGINE_VERSION_MINOR, 0),
                                     VK_API_VERSION_1_3);
        vk::InstanceCreateInfo instance_info({}, &app_info);


#if false
        std::array enabled_layer_names = {
                "VK_LAYER_KHRONOS_validation"
        };
#else
        std::array<const char *, 0> enabled_layer_names = {};
#endif

        // Enable extensions
        std::vector enabled_extension_names = {
                // VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
#ifndef NDEBUG
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
                // VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                VK_KHR_SURFACE_EXTENSION_NAME
        };
        // Append extra instance extensions
        if(extra) {
            for(int i = 0; i < (int)extra->extra_instance_extension_count; i++) {
                enabled_extension_names.push_back(extra->extra_instance_extensions[i]);
            }
        }

        auto extension_props = vk::enumerateInstanceExtensionProperties();
        auto layer_props = vk::enumerateInstanceLayerProperties();
        auto instance_version = vk::enumerateInstanceVersion();
        MI_LOG(MIInfraLogType::kInfo, "Vulkan instance version: {}.{}.{}",
               VK_VERSION_MAJOR(instance_version),
               VK_VERSION_MINOR(instance_version),
               VK_VERSION_PATCH(instance_version)
        );
        if(instance_version < MI_MIN_VULKAN_API_VERSION) {
            mi_assert(false,
                      "Required instance version is {}.{}.{}, consider update your graphics driver.",
                     VK_VERSION_MAJOR(MI_MIN_VULKAN_API_VERSION),
                     VK_VERSION_MINOR(MI_MIN_VULKAN_API_VERSION),
                     VK_VERSION_PATCH(MI_MIN_VULKAN_API_VERSION)
             );
        }
        {
            std::string available_extension_names = "Available extension names: \n";
            for (auto e: extension_props) {
                available_extension_names +=
                        std::string("    ") + std::string(e.extensionName.operator char *()) + "\n";
            }
            std::string available_layer_names = "Available layer names: \n";
            for (auto e: layer_props) {
                available_layer_names += std::string("    ") + std::string(e.layerName.operator char *()) + "\n";
            }

            for (auto extension_name: enabled_extension_names) {
                bool flag = false;
                for (auto e: extension_props)
                    if (strcmp(e.extensionName, extension_name) == 0) flag = true;
                if (!flag)
                    mi_assert(false,
                              "Required instance extension '{}' is not present, consider update your graphics driver.\n"
                              "{}",
                              extension_name, available_layer_names);
            }
            for (auto layer_name: enabled_layer_names) {
                bool flag = false;
                for (auto e: layer_props)
                    if (strcmp(e.layerName, layer_name) == 0) flag = true;
                if (!flag) {
                    std::string additional_info;
                    if (strcmp(layer_name, "VK_LAYER_KHRONOS_validation") == 0) {
                        additional_info = "This layer is provided by the Vulkan SDK, make sure you have installed it."
                                          "If you installed the validation layer with vcpkg,"
                                          " consider add the binary path to the VK_ADD_LAYER_PATH environment variable.\n";
                    }
                    mi_assert(false,
                              "Required instance layer '{}' is not present. {}\n"
                              "{}",
                              layer_name, additional_info, available_layer_names);
                }

            }
        }

        instance_info.ppEnabledExtensionNames = enabled_extension_names.data();
        instance_info.enabledExtensionCount = (uint32_t)enabled_extension_names.size();
        instance_info.ppEnabledLayerNames = enabled_layer_names.data();
        instance_info.enabledLayerCount = (uint32_t)enabled_layer_names.size();

        instance_ = vk::createInstance(instance_info);
    }

    // Initialize function dispatcher (func pointers)
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance_);

    // Select physical device
    {
        physical_device_ = nullptr;
        auto devices = instance_.enumeratePhysicalDevices();
        std::string device_names = "";
        for(auto dev : devices) {
            auto props = dev.getProperties();
            auto device_name = props.deviceName;
            device_names += std::string(device_name.operator char *()) + ", ";
        }
        MI_LOG(MIInfraLogType::kInfo, "Graphics devices: {}", device_names);
        for(auto dev : devices) {
            auto props = dev.getProperties();
            // Select the first discrete GPU
            if(props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
                physical_device_ = dev;
                break;
            }
        }
        if(!physical_device_) {
            mi_assert(false, "No discrete GPU found. "
                             "If you actually have a discrete GPU, make sure it is properly installed and with "
                             "a driver upgraded to the latest version.");
        }
        MI_LOG(MIInfraLogType::kInfo, "Selected physical device: {}", physical_device_.getProperties().deviceName.operator char *());
    }

    // Query device properties
    {
        auto props = physical_device_.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorBufferPropertiesEXT>();
        physical_device_properties_.self = props.get<vk::PhysicalDeviceProperties2>();
        physical_device_properties_.descriptor_buffer = props.get<vk::PhysicalDeviceDescriptorBufferPropertiesEXT>();
    }

    // Create a device
    {

        auto queue_family_properties = physical_device_.getQueueFamilyProperties();
        int graphics_queue_family_index = -1;
        for(int i = 0; i < queue_family_properties.size(); i++) {
            if (queue_family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
                graphics_queue_family_index = i;
                break;
            }
        }
        assert(graphics_queue_family_index != -1);
        graphics_queue_family_index_ = graphics_queue_family_index;
        std::array queue_priorities = {1.0f};
        vk::DeviceQueueCreateInfo queue_info({}, graphics_queue_family_index, queue_priorities);

        std::array enabled_extension_names = {
                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                // Support hw ray tracing
                VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                // Support hw ray tracing pipeline
                VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                // Swapchain
                VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                // Null descriptor
                VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
                // Warp ops
                // VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME,
                // VK_EXT_SHADER_SUBGROUP_VOTE_EXTENSION_NAME,
                VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
                // Dynamic pipeline states
                // VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                // uint8 indexing
                VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME,
                // Indexing device memory using addresses
                // Use the KHR version for compatibility with Nsight
                // VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                // Draw lines
                VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
                // Mesh shader support
                VK_EXT_MESH_SHADER_EXTENSION_NAME,
                // Descriptor indexing (bindless supoort)
                // VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                // Descriptor buffer (bindless support)
                VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
                // more dynamic states
                VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
#ifndef NDEBUG
                VK_KHR_RAY_QUERY_EXTENSION_NAME
#endif
                // SPV extensions (not supported by NVIDIA)
//                VK_GOOGLE_USER_TYPE_EXTENSION_NAME,
//                VK_GOOGLE_HLSL_FUNCTIONALITY1_EXTENSION_NAME,
                // Debugging
                // VK_EXT_DEBUG_MARKER_EXTENSION_NAME // Promoted to VK_EXT_debug_utils extension
                // VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME // This extension has been promoted to Vulkan Core in 1.2
        };
        // Check if the required extensions are supported
        auto supported_extensions = physical_device_.enumerateDeviceExtensionProperties();
        std::string failure_log = "";
        for(auto supported : supported_extensions) {
            failure_log += std::string((const char*)supported.extensionName) + "\n";
        }
        for(auto extension_name : enabled_extension_names) {
            bool flag = false;
            for(auto e : supported_extensions)
                if(strcmp(e.extensionName, extension_name) == 0) flag = true;
            failure_log += std::string("Enable extension: ") + extension_name + "\n";
            if(!flag) {
                mi_assert(false,
                                "Required device extension '{}' is not present, consider update your graphics driver."
                                "Current device supports: \n{}",
                                extension_name, failure_log);
            }
        }
        {
            // TODO validate physical device features
            auto features = physical_device_.getFeatures();
            assert(features.fillModeNonSolid);
        }

        vk::PhysicalDeviceFeatures enabled_features {};
        enabled_features.samplerAnisotropy = VK_TRUE;
        enabled_features.independentBlend = VK_TRUE;
        enabled_features.robustBufferAccess = VK_TRUE;
        enabled_features.fillModeNonSolid = VK_TRUE;
        enabled_features.fragmentStoresAndAtomics = VK_TRUE;
        enabled_features.geometryShader = VK_TRUE;
        enabled_features.shaderInt64 = VK_TRUE;
        enabled_features.vertexPipelineStoresAndAtomics = VK_TRUE;

        // 25.5.1: DO NOT use vk::PhysicalDeviceVulkan1xFeatures to replace the structs,
        // they trigger false positives in validation layers, potentially due to Vulkan SDK bugs.
        vk::StructureChain<vk::DeviceCreateInfo,
                vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
                vk::PhysicalDeviceMeshShaderFeaturesEXT,
                vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                vk::PhysicalDeviceRobustness2FeaturesEXT,
                vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
                vk::PhysicalDeviceIndexTypeUint8FeaturesEXT,
                vk::PhysicalDeviceShaderAtomicFloatFeaturesEXT,
                vk::PhysicalDeviceDescriptorBufferFeaturesEXT,
                vk::PhysicalDeviceDynamicRenderingFeatures,
                vk::PhysicalDeviceMaintenance4Features,
                vk::PhysicalDeviceSynchronization2Features,
                vk::PhysicalDeviceShaderDrawParametersFeatures,
                vk::PhysicalDeviceMultiviewFeatures,
                vk::PhysicalDevice16BitStorageFeatures,
                vk::PhysicalDeviceBufferDeviceAddressFeatures,
                vk::PhysicalDeviceDescriptorIndexingFeatures,
                vk::PhysicalDeviceScalarBlockLayoutFeatures,
                vk::PhysicalDeviceImagelessFramebufferFeatures,
                vk::PhysicalDeviceTimelineSemaphoreFeatures,
                vk::PhysicalDeviceFloat16Int8FeaturesKHR,
                vk::PhysicalDevice8BitStorageFeaturesKHR,
                vk::PhysicalDeviceHostQueryResetFeatures,
                vk::PhysicalDeviceRayQueryFeaturesKHR
        > extended_features;

        auto & device_create_info = std::get<0>(extended_features);
        device_create_info.setQueueCreateInfos(queue_info);
        device_create_info.setPEnabledExtensionNames(enabled_extension_names);
        device_create_info.setPEnabledFeatures(&enabled_features);

        auto & shader_draw_parameters = std::get<12>(extended_features);
        shader_draw_parameters.shaderDrawParameters = VK_TRUE;

        auto & multiview_features = std::get<13>(extended_features);
        multiview_features.multiview = VK_TRUE;

        auto & storage_16bit = std::get<14>(extended_features);
        storage_16bit.storageBuffer16BitAccess = VK_TRUE;
        storage_16bit.uniformAndStorageBuffer16BitAccess = VK_TRUE;

        auto & buffer_device_address = std::get<15>(extended_features);
        buffer_device_address.bufferDeviceAddress = VK_TRUE;

        auto & descriptor_indexing = std::get<16>(extended_features);
        descriptor_indexing.descriptorBindingPartiallyBound = VK_TRUE;
        descriptor_indexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        descriptor_indexing.runtimeDescriptorArray = VK_TRUE;

        auto & scalar_block_layout = std::get<17>(extended_features);
        scalar_block_layout.scalarBlockLayout = VK_TRUE;

        auto & imageless_framebuffer = std::get<18>(extended_features);
        imageless_framebuffer.imagelessFramebuffer = VK_TRUE;

        auto & timeline_semaphore = std::get<19>(extended_features);
        timeline_semaphore.timelineSemaphore = VK_TRUE;

        auto & float16_int8 = std::get<20>(extended_features);
        float16_int8.shaderFloat16 = VK_TRUE;
        float16_int8.shaderInt8 = VK_TRUE;

        auto & storage_8bit = std::get<21>(extended_features);
        storage_8bit.storageBuffer8BitAccess = VK_TRUE;
        storage_8bit.uniformAndStorageBuffer8BitAccess = VK_TRUE;

        auto & host_query_reset = std::get<22>(extended_features);
        host_query_reset.hostQueryReset = VK_TRUE;

        auto & RT_features = std::get<1>(extended_features);
        RT_features.rayTracingPipeline = VK_TRUE;

        auto & mesh_shader_features = std::get<2>(extended_features);
        mesh_shader_features.taskShader = VK_TRUE;
        mesh_shader_features.meshShader = VK_TRUE;

        auto & accel_features = std::get<3>(extended_features);
        accel_features.accelerationStructure = VK_TRUE;

        auto & robustness_features = std::get<4>(extended_features);
        robustness_features.nullDescriptor = VK_TRUE;
        robustness_features.robustBufferAccess2 = VK_TRUE;

        auto & dynamic_state_features = std::get<5>(extended_features);
        dynamic_state_features.extendedDynamicState3DepthClampEnable = VK_TRUE;
        dynamic_state_features.extendedDynamicState3PolygonMode = VK_TRUE;

        auto & index8 = std::get<6>(extended_features);
        index8.indexTypeUint8 = VK_TRUE;

        auto & fpatomic = std::get<7>(extended_features);
        fpatomic.shaderBufferFloat32AtomicAdd = VK_TRUE;

        auto & descb = std::get<8>(extended_features);
        descb.descriptorBuffer = VK_TRUE;

        auto & dyrend = std::get<9>(extended_features);
        dyrend.dynamicRendering = VK_TRUE;

        auto & maint4 = std::get<10>(extended_features);
        maint4.maintenance4 = VK_TRUE;

        auto & sync2 = std::get<11>(extended_features);
        sync2.synchronization2 = VK_TRUE;

        #ifndef NDEBUG
        auto & rayqry = std::get<23>(extended_features);
        rayqry.rayQuery = VK_TRUE;
        #endif

        device_ = physical_device_.createDevice(extended_features.get());
        // Initialize the Vulkan-HPP dispatcher
        VULKAN_HPP_DEFAULT_DISPATCHER.init(device_);
    }

    // Initialize device properties
    {
        auto props = physical_device_.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceSubgroupProperties>();
        auto& subgroup_props = props.get<vk::PhysicalDeviceSubgroupProperties>();

        rhi_device_properties_.wave_size = subgroup_props.subgroupSize;
        strcpy_s(rhi_device_properties_.device_name, physical_device_properties_.self.properties.deviceName);

        auto rt_props = physical_device_.getProperties2<
            vk::PhysicalDeviceProperties2,
            vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>().get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();

        rhi_device_properties_.shader_group_handle_size = rt_props.shaderGroupHandleSize;
        rhi_device_properties_.shader_group_handle_alignment = rt_props.shaderGroupHandleAlignment;
        rhi_device_properties_.shader_group_base_alignment = rt_props.shaderGroupBaseAlignment;
        rhi_device_properties_.max_ray_recursion_depth = rt_props.maxRayRecursionDepth;
        rhi_device_properties_.max_shader_group_stride = rt_props.maxShaderGroupStride;
    }

    // Device resources
    {
        queue_ = device_.getQueue(graphics_queue_family_index_, 0);
        LoadPipelineCache();
    }
    vma_ = vma::createAllocator(vma::AllocatorCreateInfo{
            vma::AllocatorCreateFlagBits::eKhrDedicatedAllocation // Vulkan 1.1
            | vma::AllocatorCreateFlagBits::eBufferDeviceAddress // Vulkan 1.2
            | vma::AllocatorCreateFlagBits::eKhrBindMemory2   // Vulkan 1.1
            | vma::AllocatorCreateFlagBits::eKhrMaintenance4, // Vulkan 1.3
            physical_device_,
            device_,
            C::kRHIPreferredGPUHeapBlockSize,
            nullptr, // No allocation callback
            nullptr, // No device memory callback
            nullptr, // No heap size limit
            nullptr, // Use default vulkan dispatcher for vulkan function calls
            instance_,
            MI_MIN_VULKAN_API_VERSION
    });

    // Initialize struct for export handles
    export_handles_ = {
        instance_,
        device_,
        physical_device_
    };
}

static const char* PIPELINE_CACHE_FILE_PATH = "pipeline_cache.bin";

void VulkanRHI::InvalidateDiskPipelineCache() {
    if (std::remove(PIPELINE_CACHE_FILE_PATH) == 0) {
        MI_LOG(MIInfraLogType::kInfo, "Pipeline cache file '{}' deleted.", PIPELINE_CACHE_FILE_PATH);
    } else {
        // It's okay if the file doesn't exist, so only log actual errors if needed,
        // or just ignore if std::remove returns non-zero for "file not found".
        // perror can give more info on std::remove failure.
        // MI_LOG(MIInfraLogType::kWarning, "Error deleting pipeline cache file '{}'.", PIPELINE_CACHE_FILE_PATH);
    }
}

void VulkanRHI::LoadPipelineCache() {
    std::vector<char> cache_data;
    std::ifstream cache_file(PIPELINE_CACHE_FILE_PATH, std::ios::binary | std::ios::ate);

    if (cache_file.is_open()) {
        size_t file_size = cache_file.tellg();
        cache_file.seekg(0, std::ios::beg);
        cache_data.resize(file_size);
        cache_file.read(cache_data.data(), file_size);
        cache_file.close();
        MI_LOG(MIInfraLogType::kInfo, "Pipeline cache loaded from '{}', size: {} KB.", PIPELINE_CACHE_FILE_PATH, file_size / 1024);
    } else {
        MI_LOG(MIInfraLogType::kInfo, "Pipeline cache file '{}' not found. Creating new cache.", PIPELINE_CACHE_FILE_PATH);
    }

    vk::PipelineCacheCreateInfo cache_create_info;
    if (!cache_data.empty()) {
        // Vulkan spec: The data is valid if the first 16 bytes match the header structure.
        // We can pass it directly; Vulkan will validate.
        cache_create_info.initialDataSize = cache_data.size();
        cache_create_info.pInitialData = cache_data.data();
    }

    auto result = device_.createPipelineCache(cache_create_info);
    if (!result) {
        MI_LOG(MIInfraLogType::kWarning, "Failed to load pipeline cache from disk (the binary is corrupted?). Creating empty cache.");
        // Fallback to creating an empty cache if loading/creating with data failed
        pipeline_cache_ = device_.createPipelineCache(vk::PipelineCacheCreateInfo());
    } else {
        pipeline_cache_ = result;
    }
}

VulkanRHI::~VulkanRHI() {
    queue_.waitIdle();

    // Release the resources held by upper layers first
    delete this->bindless_manager_;
    delete this->command_executor_;

    // Save pipeline cache before destroying it
    if (pipeline_cache_) {
        auto result = device_.getPipelineCacheData(pipeline_cache_);
        if (!result.empty()) {
            std::ofstream cache_file(PIPELINE_CACHE_FILE_PATH, std::ios::binary | std::ios::trunc);
            if (cache_file.is_open()) {
                cache_file.write(reinterpret_cast<const char*>(result.data()), result.size());
                cache_file.close();
                MI_LOG(MIInfraLogType::kInfo, "Pipeline cache saved to '{}', size: {} KB.", PIPELINE_CACHE_FILE_PATH, result.size() / 1024);
            } else {
                MI_LOG(MIInfraLogType::kWarning, "Failed to open pipeline cache file '{}' for writing.", PIPELINE_CACHE_FILE_PATH);
            }
        } else {
            MI_LOG(MIInfraLogType::kWarning, "Failed to get pipeline cache data.");
        }
    }

    // Release swapchain (if present)
    if(swapchain_) {
        // The RHI thread is stopped later, so automatic resource recycling is still functional now.
        for (auto & e : rhi_backbuffer_textures) e.SafeRelease();
        device_.destroy(swapchain_);
        // Release semaphores
        for (int i = 0; i < (int)swapchain_images.size(); i++) {
            device_.destroy(vk_swapchain_image_available_semaphores_[i]);
            device_.destroy(vk_swapchain_render_finished_semaphores_[i]);
        }
        swapchain_images.clear();
    }

    // If the surface is created, destroy it
    if(surface_) {
        instance_.destroy(surface_);
        surface_ = nullptr;
    }

    // Recycle unused resources
    EnqueueRHIThreadTask([this](){
        RecycleRHIResourcesPendingForDeletion_RHIThread(true);
    }).wait();

    // We can safely destroy device resources now
    vma_.destroy();
    device_.destroy(pipeline_cache_);
    device_.destroy();
    instance_.destroy();

    // The RHI thread is stopped later.
}

RHIDeviceProperties VulkanRHI::GetDeviceProperties() const {
    assert(rhi_device_properties_.wave_size != 0);
    return rhi_device_properties_;
}

bool VulkanRHI::InitializeSwapChain_RHI(const void *surface_handle_ptr, uint32_t width, uint32_t height, uint32_t * out_swapchain_size) {

    assert(surface_ == nullptr);

    vk::SurfaceKHR surface = *(const vk::SurfaceKHR*)surface_handle_ptr;
    surface_ = surface;

    auto capabilities = physical_device_.getSurfaceCapabilitiesKHR(surface);
    auto formats = physical_device_.getSurfaceFormatsKHR(surface);
    auto present_modes = physical_device_.getSurfacePresentModesKHR(surface);

    vk::SurfaceFormatKHR surface_format = formats[0];
    for (const auto& format : formats) {
        if (format.format == vk::Format::eB8G8R8A8Srgb &&
            format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            surface_format = format;
            break;
        }
    }

    vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
    for (const auto& mode : present_modes) {
        if (mode == vk::PresentModeKHR::eMailbox) {
            present_mode = mode;
            break;
        }
    }

    vk::Extent2D extent;
    if (capabilities.currentExtent.width != UINT32_MAX) {
        extent = capabilities.currentExtent;
    } else {
        extent = vk::Extent2D{width, height};
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    vk::SwapchainCreateInfoKHR create_info{};
    create_info.surface = surface;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    // Swapchain images are only being copied to
    create_info.imageUsage = vk::ImageUsageFlagBits::eTransferDst
    // 25.4.19 However, to be compatiable with NSight Graphics, extra usages are required
    // otherwise it will silently present wrong images when visualizing rasterization
        | vk::ImageUsageFlagBits::eColorAttachment;

    uint32_t graphics_queue_family_index = GetGraphicsQueueFamilyIndex();
    // The same for now
    uint32_t present_queue_family_index = graphics_queue_family_index;

    if (graphics_queue_family_index != present_queue_family_index) {
        create_info.imageSharingMode = vk::SharingMode::eConcurrent;
        create_info.queueFamilyIndexCount = 2;
        uint32_t queue_family_indices[] = {graphics_queue_family_index, present_queue_family_index};
        create_info.pQueueFamilyIndices = queue_family_indices;
    } else {
        create_info.imageSharingMode = vk::SharingMode::eExclusive;
    }

    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    create_info.oldSwapchain = nullptr;

    swapchain_ = device_.createSwapchainKHR(create_info);

    swapchain_images = device_.getSwapchainImagesKHR(swapchain_);
    if (out_swapchain_size) {
        *out_swapchain_size = (uint32_t)swapchain_images.size();
    }

    // Set up semaphores for presentation
    {
        vk_swapchain_image_available_semaphores_.resize(swapchain_images.size());
        vk_swapchain_render_finished_semaphores_.resize(swapchain_images.size());
        for (int i = 0; i < (int)swapchain_images.size(); i++) {
            vk::SemaphoreCreateInfo semaphore_info {};
            vk_swapchain_image_available_semaphores_[i] = device_.createSemaphore(semaphore_info);
            vk_swapchain_render_finished_semaphores_[i] = device_.createSemaphore(semaphore_info);
            // Set debug names of the semaphores
            std::string name = "Swapchain image available semaphore " + std::to_string(i);
            vk::DebugUtilsObjectNameInfoEXT name_info {};
            name_info.objectType = vk::ObjectType::eSemaphore;
            name_info.objectHandle = (uint64_t)(VkSemaphore)vk_swapchain_image_available_semaphores_[i];
            name_info.pObjectName = name.c_str();
            vk::DebugUtilsObjectNameInfoEXT name_info2 {};
            name_info2.objectType = vk::ObjectType::eSemaphore;
            name_info2.objectHandle = (uint64_t)(VkSemaphore)vk_swapchain_render_finished_semaphores_[i];
            name_info2.pObjectName = name.c_str();
            device_.setDebugUtilsObjectNameEXT(name_info);
            device_.setDebugUtilsObjectNameEXT(name_info2);

            vk::DebugUtilsObjectNameInfoEXT name_info3 {};
            name_info3.objectType = vk::ObjectType::eImage;
            name_info3.objectHandle = (uint64_t)(VkImage)swapchain_images[i];
            name_info3.pObjectName = (std::string("Swapchain image ") + std::to_string(i)).c_str();
            device_.setDebugUtilsObjectNameEXT(name_info3);
        }
    }

    // Create back buffers (always 2)
    for (size_t i = 0; i < 2; i++) {
        auto tex = new VulkanTexture(RHITextureType::k2D, {width, height, 1},
                            GetPixelFormatFromVulkanFormat(surface_format.format),
                            RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferSrc
                            | RHITextureUsageFlagBits::kTransferDst | RHITextureUsageFlagBits::kShaderResource, 1, 1
        );
        tex->SetName("BackBuffer#" + std::to_string(i));
        rhi_backbuffer_textures[i] = tex;
    }

    return true;
}

RHITexture *VulkanRHI::GetBackBufferForFrameIndex(size_t index) const {
    return rhi_backbuffer_textures[index & 1].Raw();
}


RHIBufferRef VulkanRHI::CreateBuffer(RHIBufferDesc desc) {
    auto buffer = new VulkanBuffer(desc);
    return TRef<RHIBuffer>(buffer);
}

RHITextureRef VulkanRHI::CreateTexture(RHITextureDesc desc) {
    auto texture = new VulkanTexture(desc);
    return TRef<RHITexture>(texture);
}

RHISamplerRef VulkanRHI::CreateSampler(RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) {
    auto sampler = new VulkanSampler(filter, address_mode);
    return TRef<RHISampler>(sampler);
}

RHIShaderRef VulkanRHI::CreateShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                     RHIShaderIRType ir_type, std::span<const std::byte> ir) {
    mi_assert(ir_type == RHIShaderIRType::kSPIRV, "Vulkan only supports SPIR-V shader IR.");
    auto shader = new VulkanShader(frequency, entry_name, ir_type, ir);
    shader->Compile();
    if(shader->IsValid()) return TRef<RHIShader>(shader);
    shader->~VulkanShader();
    delete shader;
    return nullptr;
}

RHIGraphicsPipelineRef VulkanRHI::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc &desc, const char * name) {
    auto pipeline = new VulkanGraphicsPipeline();
    pipeline->Compile(desc);
    pipeline->SetName(name);
    if(pipeline->IsValid()) return TRef<RHIGraphicsPipeline>(pipeline);
    pipeline->~VulkanGraphicsPipeline();
    delete pipeline;
    return nullptr;
}

RHIComputePipelineRef VulkanRHI::CreateComputePipeline(RHIShader *shader, const char * name) {
    auto pipeline = new VulkanComputePipeline();
    pipeline->Compile(shader);
    pipeline->SetName(name);
    if(pipeline->IsValid()) return TRef<RHIComputePipeline>(pipeline);
    pipeline->~VulkanComputePipeline();
    delete pipeline;
    return nullptr;
}

void VulkanRHI::ResetPipelineCache() {
    device_.destroy(pipeline_cache_);
    InvalidateDiskPipelineCache();
    LoadPipelineCache();
}

RHIBindlessSupportInfo VulkanRHI::QueryRHIBindlessSupportInfo() {
    auto descriptor_props = physical_device_properties_.descriptor_buffer;
    RHIBindlessSupportInfo info {};
    info.max_num_resource_slots = descriptor_props.maxResourceDescriptorBufferBindings;
    // info.max_num_sampler_slots  = descriptor_props.maxSamplerDescriptorBufferBindings;
    // info.max_num_immutable_sampler_slots = descriptor_props.maxEmbeddedImmutableSamplers;
    info.descriptor_buffer_offset_alignment   = (uint32_t)descriptor_props.descriptorBufferOffsetAlignment;
    return info;
}

uint32_t VulkanRHI::GetGraphicsQueueFamilyIndex() {
    return graphics_queue_family_index_;
}

uint32_t VulkanRHI::GetQueueFamilyIndex([[maybe_unused]] RHICommandQueueType type) {
    assert(type == RHICommandQueueType::kGraphics);
    return graphics_queue_family_index_;
}

RHICommandExecutorInterface * VulkanRHI::GetCommandExecutor() {
    return command_executor_;
}

void VulkanRHI::WaitForIdle(bool host_only) {
    assert(IsRenderThread());
    // Simply wait the RHI thread to finish its work
    auto fut = EnqueueRHIThreadTask([]() {});
    fut.wait();

    if(!host_only) {
        queue_.waitIdle();
    }
}

RHITextureRef VulkanRHI::ImportTexture(const void * raw_desc, RHITextureType type, RHITextureDimensions dimensions,
                                       PixelFormatType format, RHITextureUsageFlags usage, int mip_levels,
                                       int array_layers) {
    auto desc = (const VulkanTextureImportDesc *)raw_desc;
    auto texture = new VulkanTexture(type, dimensions, format, usage, mip_levels, array_layers, true);
    vk::Image image_handle = {(VkImage)desc->vk_image};
    vk::ImageLayout layout = (vk::ImageLayout)(desc->vk_image_layout);
    texture->ImportFromHandle(image_handle, layout);
    return TRef<RHITexture>(texture);
}

const void *VulkanRHI::GetUnderlyingGraphicsAPIHandles() const {
    return & export_handles_;
}

RHISyncPointRef VulkanRHI::CreateSyncPoint() {
    auto ptr = new VulkanSyncPoint();
    return TRef<RHISyncPoint>(ptr);
}

void VulkanRHI::PostInitialize() {
    RHI::PostInitialize();
    // Create bindless manager and command executor
    {
        mi_assert(IsRHIThreadActive(), "RHI thread must be active when creating VulkanRHI.");
        // Initialization are automatically dispatched to the RHI thread
        // via the constructor functions
        bindless_manager_ = new VulkanBindlessManager();
        command_executor_ = new VulkanCommandExecutor();
    }
}

// Shortcut to get VulkanRHI instance
VulkanRHI * GetVulkanRHI () {
    return static_cast<VulkanRHI*>(&(RHI::Get())); // NOLINT this is safe
}

// Implement factory function declared in vk_rhi_export.h
VulkanRHI * CreateVulkanRHI (const VulkanRHICreateInfo * extra) {
    return new VulkanRHI(extra);
}



MI_NAMESPACE_END