/*
 * Created: 2024/7/3
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <string>
#include "rhi/rhi.h"
#include "core/infra.h"

#include "vk_rhi.h"
#include "vk_resource.h"
#include "vk_buffer.h"
#include "vk_texture.h"
#include "vk_shader.h"
#include "vk_pipeline.h"
#include "vk_bindless.h"

MI_NAMESPACE_BEGIN

VulkanRHI::VulkanRHI() {
    {
        vk::DynamicLoader dl;
        auto vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

        vk::ApplicationInfo app_info("MIRenderer", MI_APPLICATION_VERSION, MI_ENGINE_NAME, MI_ENGINE_VERSION,
                                     VK_API_VERSION_1_3);
        vk::InstanceCreateInfo instance_info({}, &app_info);

        // Enable layers
#ifndef NDEBUG
        std::array enabled_layer_names = {
                "VK_LAYER_KHRONOS_validation"
        };
#else
        std::array<const char *, 0> enabled_layers = {};
#endif

        // Enable extensions
        std::array enabled_extension_names = {
                VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
#ifndef NDEBUG
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                VK_KHR_SURFACE_EXTENSION_NAME
        };

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
            std::string layer_names = "Available layer names: \n";
            for (auto e: layer_props) {
                layer_names += std::string("    ") + std::string(e.layerName.operator char *()) + "\n";
            }
            MI_LOG(MIInfraLogType::kInfo, "{}; {}", available_extension_names, layer_names);

            for (auto extension_name: enabled_extension_names) {
                bool flag = false;
                for (auto e: extension_props)
                    if (strcmp(e.extensionName, extension_name) == 0) flag = true;
                MI_LOG(MIInfraLogType::kInfo, "Enable extension: {}", extension_name);
                if (!flag)
                    mi_assert(false,
                            "Required instance extension '{}' is not present, consider update your graphics driver.",
                            extension_name);
            }
        }

        for(auto layer_name : enabled_layer_names) {
            bool flag = false;
            for(auto e : layer_props)
                if(strcmp(e.layerName, layer_name) == 0) flag = true;
            MI_LOG(MIInfraLogType::kInfo, "Enable layer: {}", layer_name);
            if(!flag) mi_assert(false,
                                "Required instance layer '{}' is not present, consider update your graphics driver.",
                                layer_name);

        }

        instance_ = vk::createInstance(instance_info);
    }
    // Select physical device
    {
        physical_device_ = instance_.enumeratePhysicalDevices().front();
        std::string device_name = physical_device_.getProperties().deviceName;
        MI_LOG(MIInfraLogType::kInfo, "Selected physical device: {}", device_name);
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
        uint32_t graphics_queue_family_index = -1;
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
                VK_EXT_SHADER_SUBGROUP_BALLOT_EXTENSION_NAME,
                VK_EXT_SHADER_SUBGROUP_VOTE_EXTENSION_NAME,
//                    VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
                // Dynamic pipeline states
                VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
                // Indexing device memory using addresses
                VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                // Draw lines
                VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
                // Mesh shader support
                VK_EXT_MESH_SHADER_EXTENSION_NAME,
                // Debugging
                VK_EXT_DEBUG_MARKER_EXTENSION_NAME
                // VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME // This extension has been promoted to Vulkan Core in 1.2
        };
        vk::PhysicalDeviceFeatures enabled_features {};
        enabled_features.independentBlend = VK_TRUE; // Blend mode being identical for each color attachment
        enabled_features.robustBufferAccess = VK_TRUE; // Robust buffer access (fill missing vertex data)
        enabled_features.fillModeNonSolid = VK_TRUE; // Draw lines
        enabled_features.fragmentStoresAndAtomics = VK_TRUE; // Used by some algorithms
        enabled_features.geometryShader = VK_TRUE;
        enabled_features.shaderInt64 = VK_TRUE; // Required by acceleration structure & buffer reference
        vk::StructureChain<vk::DeviceCreateInfo,
                vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
                vk::PhysicalDeviceMeshShaderFeaturesNV,
                vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
                vk::PhysicalDeviceRobustness2FeaturesEXT,
                vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                vk::PhysicalDeviceDescriptorIndexingFeatures,
                vk::PhysicalDeviceBufferDeviceAddressFeatures,
                vk::PhysicalDevice16BitStorageFeatures,
                vk::PhysicalDevice8BitStorageFeatures,
                vk::PhysicalDeviceIndexTypeUint8FeaturesEXT,
                vk::PhysicalDeviceMaintenance4Features,
                vk::PhysicalDeviceShaderFloat16Int8Features,
                vk::PhysicalDeviceSynchronization2Features,
                vk::PhysicalDeviceScalarBlockLayoutFeatures,
                vk::PhysicalDeviceHostQueryResetFeatures,
                vk::PhysicalDeviceShaderAtomicFloatFeaturesEXT,
                vk::PhysicalDeviceTimelineSemaphoreFeatures
        > extended_features;
        auto & device_create_info = std::get<0>(extended_features);
        device_create_info.setQueueCreateInfos(queue_info);
        device_create_info.setPEnabledExtensionNames(enabled_extension_names);
        device_create_info.setPEnabledFeatures(&enabled_features);
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
        dynamic_state_features.extendedDynamicState = VK_TRUE;
        auto & descriptor_set_indexing = std::get<6>(extended_features);
        // Allow descriptors to be partially bound
        descriptor_set_indexing.descriptorBindingPartiallyBound = VK_TRUE;
        auto & buffer_device_addr = std::get<7>(extended_features);
        buffer_device_addr.bufferDeviceAddress = VK_TRUE;
        auto & storage16 = std::get<8>(extended_features);
        storage16.storageBuffer16BitAccess = VK_TRUE;
        storage16.uniformAndStorageBuffer16BitAccess = VK_TRUE;
        auto & storage8 = std::get<9>(extended_features);
        storage8.storageBuffer8BitAccess = VK_TRUE;
        storage8.uniformAndStorageBuffer8BitAccess = VK_TRUE;
        auto & index8 = std::get<10>(extended_features);
        index8.indexTypeUint8 = VK_TRUE;
        auto & maintenance4 = std::get<11>(extended_features);
        maintenance4.maintenance4 = VK_TRUE;
        auto & f16i8 = std::get<12>(extended_features);
        f16i8.shaderInt8 = VK_TRUE;
        f16i8.shaderFloat16 = VK_TRUE;
        auto & sync2 = std::get<13>(extended_features);
        sync2.synchronization2 = VK_TRUE;
        auto & scalar = std::get<14>(extended_features);
        scalar.scalarBlockLayout = VK_TRUE;
        auto & hostq = std::get<15>(extended_features);
        hostq.hostQueryReset = VK_TRUE;
        auto & fpatomic = std::get<16>(extended_features);
        fpatomic.shaderBufferFloat32AtomicAdd = VK_TRUE;
#ifndef NDEBUG
        auto & timesem = std::get<17>(extended_features);
        timesem.timelineSemaphore = VK_TRUE;
#endif

        device_ = physical_device_.createDevice(extended_features.get());
        // Initialize the Vulkan-HPP dispatcher
        VULKAN_HPP_DEFAULT_DISPATCHER.init(device_);
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
            CRHIPreferredGPUHeapBlockSize,
            nullptr, // No allocation callback
            nullptr, // No device memory callback
            nullptr, // No heap size limit
            nullptr, // Use default vulkan dispatcher for vulkan function calls
            instance_,
            MI_MIN_VULKAN_API_VERSION
    });

    // Factory calls
    {
        bindless_manager_ = new VulkanBindlessManager();
    }
}

void VulkanRHI::InvalidateDiskPipelineCache() {
    // TODO
}

void VulkanRHI::LoadPipelineCache() {
    // TODO actually load from disk
    pipeline_cache_ = device_.createPipelineCache(vk::PipelineCacheCreateInfo());
}

VulkanRHI::~VulkanRHI() {
    queue_.waitIdle();
    device_.destroy(pipeline_cache_);
    device_.destroy();
    instance_.destroy();
    vma_.destroy();
}

RHIBufferRef VulkanRHI::CreateBuffer(size_t size, RHIBufferUsageFlagBits type, RHIGPUAccessFlagBits access_type) {
    auto buffer = new VulkanBuffer(size, type,access_type);
    return {buffer};
}

RHITextureRef VulkanRHI::CreateTexture(RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
                                       RHITextureUsageFlags usage, int mip_levels, int array_layers) {
    auto texture = new VulkanTexture(type, dimensions, format, usage, mip_levels, array_layers);
    return {texture};
}

RHISamplerRef VulkanRHI::CreateSampler(RHISamplerFilterType filter, RHISamplerAddressModeType address_mode) {
    auto sampler = new VulkanSampler(filter, address_mode);
    return {sampler};
}

RHIShaderRef VulkanRHI::CreateShader(RHIShaderFrequencyFlagBits frequency, std::string_view entry_name,
                                     RHIShaderIRType ir_type, std::span<const std::byte> ir) {
    mi_assert(ir_type == RHIShaderIRType::kSPIRV, "Vulkan only supports SPIR-V shader IR.");
    auto shader = new VulkanShader(frequency, entry_name, ir_type, ir);
    shader->Compile();
    if(shader->IsValid()) return {shader};
    delete shader;
    return nullptr;
}

RHIGraphicsPipelineRef VulkanRHI::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc &desc) {
    auto pipeline = new VulkanGraphicsPipeline("");
    pipeline->Compile(desc);
    if(pipeline->IsValid()) return pipeline;
    delete pipeline;
    return nullptr;
}

RHIComputePipelineRef VulkanRHI::CreateComputePipeline(RHIShader *shader) {
    auto pipeline = new VulkanComputePipeline("");
    pipeline->Compile(shader);
    if(pipeline->IsValid()) return pipeline;
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
    info.max_resource_slots = descriptor_props.maxResourceDescriptorBufferBindings;
    info.max_sampler_slots  = descriptor_props.maxSamplerDescriptorBufferBindings;
    info.max_immutable_sampler_slots = descriptor_props.maxEmbeddedImmutableSamplers;
    info.descriptor_buffer_offset_alignment   = descriptor_props.descriptorBufferOffsetAlignment;
}

static VulkanRHI * CreateVulkanRHI() {
    return new VulkanRHI();
}

VulkanRHI * GetVulkanRHI () {
    return static_cast<VulkanRHI*>(&(RHI::GetInstance())); // NOLINT this is safe
}



MI_NAMESPACE_END