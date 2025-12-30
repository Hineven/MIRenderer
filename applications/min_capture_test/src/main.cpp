// 此文件用于提供一个minimal case模拟目前3d_viewer对vulkan api的调用情况，从而复现3d_viewer的每帧vulkan-1.dll内内存持续增长的bug，调查原因。

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>
#include <vk_mem_alloc.hpp>

#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>

#include <iostream>
#include <vector>
#include <array>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstring>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static const uint32_t WIDTH = 800;
static const uint32_t HEIGHT = 600;

// 实验开关：逐个打开以二分定位
static constexpr uint32_t kFramesInFlight = 2;

// 新增：用 command pool reset 来模拟 renderer 的行为（而不是 reset command buffer）
static constexpr bool kResetCommandPoolPerFrame = true; // !!!!!!!!!!!! <- this triggers the bug!

// 对齐 MIRenderer: 每帧 reset descriptor pool，并进行一定量的 descriptor alloc/update
static constexpr bool kEnablePerFrameDescriptorPoolReset = true;
static constexpr uint32_t kDescriptorSetsPerFrame = 256;
static constexpr uint32_t kWritesPerFrame = 256;

// 对齐 VulkanBindlessManager: 双缓冲 descriptor set + 每帧 CopyDescriptorSet 全量拷贝
static constexpr bool kEnableBindlessLikeCopyPerFrame = true;

// B: 对齐 vk_constants.h 的 descriptor 上限
static constexpr uint32_t kDescriptorPoolMaxSetsPerFrame = 512;
static constexpr uint32_t kDescriptorPoolMaxStorageBufferDescsPerFrame = 2048;

// C: timestamp/query pool
static constexpr bool kEnableTimestampQuery = true;
static constexpr uint32_t kMaxNumTimestampQueries = 2048;
static constexpr uint32_t kTimestampQueriesPerFrame = kMaxNumTimestampQueries / kFramesInFlight;
static constexpr uint32_t kTimestampsToWritePerFrame = 128;

// D: bindless free + null writes + copy
// Align to API dump: 3 bindings copied every frame: binding0=1024, binding1=1024, binding2=16
static constexpr uint32_t kBindlessBinding0Count = 1024;
static constexpr uint32_t kBindlessBinding1Count = 1024;
static constexpr uint32_t kBindlessBinding2Count = 16;
static constexpr uint32_t kBindlessTotalSlots = kBindlessBinding0Count + kBindlessBinding1Count + kBindlessBinding2Count;

// For slot-free emulation (kept for compatibility with earlier repro code)
static constexpr uint32_t kBindlessFreeSlotsPerFrame = 256;

// Keep for other per-frame updates that use a single binding index
static constexpr uint32_t kBindlessTableSize = kBindlessTotalSlots;

// E: 对齐 vk_cmd_exec.cpp 的 frame-end：backbuffer->swapchain copy + per-swapchain-image semaphores
static constexpr bool kEnableFrameEndCopyToSwapchain = true;
static constexpr bool kEnableExecutionBarrier = true;
static constexpr bool kEnableMegaBarrier = false;

// VMA pressure knobs
static constexpr bool kEnableVma = true;
static constexpr bool kVmaAllocFreePerFrame = false;
static constexpr uint32_t kVmaBuffersPerFrame = 128;
static constexpr vk::DeviceSize kVmaBufferSize = 4 * 1024; // 4KB-16KB 小对象更容易触发 driver/path 行为
static constexpr bool kVmaUseDedicatedSometimes = true;
static constexpr uint32_t kVmaDedicatedModulo = 32; // 每 N 次做一次 dedicated 分配

int main() {

    // 动态 dispatcher 初始化（vulkan-hpp dynamic loader）
    VULKAN_HPP_DEFAULT_DISPATCHER.init();

    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "MinCaptureTest", nullptr, nullptr);
    if (!window) { std::cerr << "Failed to create window\n"; return 1; }

    // Instance
    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "MinCaptureTest";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_4;
    std::vector<const char*> instExts;
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    for (uint32_t i = 0; i < glfwExtCount; ++i) {
        // Plain runtime pointer copy; keep it simple to avoid clangd/libstdc++ constexpr diagnostics.
        instExts.push_back((const char*)glfwExts[i]);
    }
#ifndef NDEBUG
    instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    vk::InstanceCreateInfo instInfo({}, &appInfo, 0, nullptr, (uint32_t)instExts.size(), instExts.data());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
    vk::Instance instance = vk::createInstance(instInfo);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

    // Physical device
    auto devices = instance.enumeratePhysicalDevices();
    if (devices.empty()) { std::cerr << "No physical device\n"; return 1; }
    vk::PhysicalDevice phys = devices[0];
    for (auto d : devices) {
        if (d.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) { phys = d; break; }
    }

    // Cache memory props early (used by helpers later)
    const auto memProps = phys.getMemoryProperties();

    // Surface
    VkSurfaceKHR c_surface{};
    if (glfwCreateWindowSurface(instance, window, nullptr, &c_surface) != VK_SUCCESS) {
        std::cerr << "Failed to create surface\n"; return 1;
    }
    vk::SurfaceKHR surface{c_surface};

    // Queue family
    auto qprops = phys.getQueueFamilyProperties();
    uint32_t gfxIndex = UINT32_MAX;
    for (uint32_t i = 0; i < qprops.size(); ++i) {
        if (qprops[i].queueFlags & vk::QueueFlagBits::eGraphics) { gfxIndex = i; break; }
    }
    if (gfxIndex == UINT32_MAX) { std::cerr << "No graphics queue\n"; return 1; }

    // Device (try to align with 3d_viewer mi/rhi/vk/vk_rhi.cpp)
    std::vector<const char*> devExts = {
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        // Null descriptor
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
        // Warp/atomic float/etc
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
        VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME,
        VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME
    };

    // Filter extensions by support so the minimal repro runs on more machines.
    // (If you want strict parity, we can flip this to assert like vk_rhi.cpp does.)
    {
        auto supported = phys.enumerateDeviceExtensionProperties();
        auto isSupported = [&](const char* name) {
            for (const auto& e : supported) {
                if (std::strcmp(e.extensionName, name) == 0) return true;
            }
            return false;
        };
        devExts.erase(std::remove_if(devExts.begin(), devExts.end(), [&](const char* n){ return !isSupported(n); }), devExts.end());
    }

    vk::PhysicalDeviceFeatures feats{};
    feats.samplerAnisotropy = VK_TRUE;
    feats.independentBlend = VK_TRUE;
    feats.robustBufferAccess = VK_TRUE;
    feats.fillModeNonSolid = VK_TRUE;
    feats.fragmentStoresAndAtomics = VK_TRUE;
    feats.geometryShader = VK_TRUE;
    feats.shaderInt64 = VK_TRUE;
    feats.vertexPipelineStoresAndAtomics = VK_TRUE;
    feats.tessellationShader = VK_TRUE;
    feats.multiDrawIndirect = VK_TRUE;
    feats.drawIndirectFirstInstance = VK_TRUE;

    vk::StructureChain<vk::DeviceCreateInfo,
        vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
        vk::PhysicalDeviceMeshShaderFeaturesEXT,
        vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
        vk::PhysicalDeviceRobustness2FeaturesEXT,
        vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
        vk::PhysicalDeviceIndexTypeUint8FeaturesEXT,
        vk::PhysicalDeviceShaderAtomicFloatFeaturesEXT,
        vk::PhysicalDeviceDynamicRenderingFeatures,
        vk::PhysicalDeviceMaintenance4Features,
        vk::PhysicalDeviceMaintenance5Features,
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
        vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR,
        vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR,
        vk::PhysicalDeviceHostQueryResetFeatures,
        vk::PhysicalDeviceRayQueryFeaturesKHR,
        vk::PhysicalDeviceVulkanMemoryModelFeatures,
        vk::PhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR
    > devChain;
    auto& dci = std::get<0>(devChain);
    dci.setPEnabledFeatures(&feats);

    std::array<float,1> priorities{1.0f};
    vk::DeviceQueueCreateInfo qci{};
    qci.queueFamilyIndex = gfxIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = priorities.data();

    dci.setQueueCreateInfos(qci);
    dci.setPEnabledExtensionNames(devExts);

    // Features: enable best-effort (if unsupported, driver will ignore/validation may complain; ok for repro)
    std::get<vk::PhysicalDeviceShaderDrawParametersFeatures>(devChain).shaderDrawParameters = VK_TRUE;
    std::get<vk::PhysicalDeviceMultiviewFeatures>(devChain).multiview = VK_TRUE;

    auto& storage16 = std::get<vk::PhysicalDevice16BitStorageFeatures>(devChain);
    storage16.storageBuffer16BitAccess = VK_TRUE;
    storage16.uniformAndStorageBuffer16BitAccess = VK_TRUE;

    auto& bdaFeat = std::get<vk::PhysicalDeviceBufferDeviceAddressFeatures>(devChain);
    bdaFeat.bufferDeviceAddress = VK_TRUE;
    bdaFeat.bufferDeviceAddressCaptureReplay = VK_TRUE;

    auto& descriptorIndexing = std::get<vk::PhysicalDeviceDescriptorIndexingFeatures>(devChain);
    descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
    descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    descriptorIndexing.runtimeDescriptorArray = VK_TRUE;

    std::get<vk::PhysicalDeviceScalarBlockLayoutFeatures>(devChain).scalarBlockLayout = VK_TRUE;
    std::get<vk::PhysicalDeviceImagelessFramebufferFeatures>(devChain).imagelessFramebuffer = VK_TRUE;
    std::get<vk::PhysicalDeviceTimelineSemaphoreFeatures>(devChain).timelineSemaphore = VK_TRUE;

    auto& float16int8 = std::get<vk::PhysicalDeviceFloat16Int8FeaturesKHR>(devChain);
    float16int8.shaderFloat16 = VK_TRUE;
    float16int8.shaderInt8 = VK_TRUE;

    auto& storage8 = std::get<vk::PhysicalDevice8BitStorageFeaturesKHR>(devChain);
    storage8.storageBuffer8BitAccess = VK_TRUE;
    storage8.uniformAndStorageBuffer8BitAccess = VK_TRUE;

    std::get<vk::PhysicalDeviceDynamicRenderingFeatures>(devChain).dynamicRendering = VK_TRUE;
    std::get<vk::PhysicalDeviceMaintenance4Features>(devChain).maintenance4 = VK_TRUE;
    std::get<vk::PhysicalDeviceMaintenance5Features>(devChain).maintenance5 = VK_TRUE;
    std::get<vk::PhysicalDeviceSynchronization2Features>(devChain).synchronization2 = VK_TRUE;

    auto& rtp = std::get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>(devChain);
    rtp.rayTracingPipeline = VK_TRUE;
    rtp.rayTracingPipelineTraceRaysIndirect = VK_TRUE;

    auto& ms = std::get<vk::PhysicalDeviceMeshShaderFeaturesEXT>(devChain);
    ms.taskShader = VK_TRUE;
    ms.meshShader = VK_TRUE;

    auto& asFeat = std::get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>(devChain);
    asFeat.accelerationStructure = VK_TRUE;
    asFeat.accelerationStructureCaptureReplay = VK_TRUE;

    auto& robust2 = std::get<vk::PhysicalDeviceRobustness2FeaturesEXT>(devChain);
    robust2.nullDescriptor = VK_TRUE;
    robust2.robustBufferAccess2 = VK_TRUE;

    auto& dyn3 = std::get<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>(devChain);
    dyn3.extendedDynamicState3DepthClampEnable = VK_TRUE;
    dyn3.extendedDynamicState3PolygonMode = VK_TRUE;

    std::get<vk::PhysicalDeviceIndexTypeUint8FeaturesEXT>(devChain).indexTypeUint8 = VK_TRUE;
    std::get<vk::PhysicalDeviceShaderAtomicFloatFeaturesEXT>(devChain).shaderBufferFloat32AtomicAdd = VK_TRUE;

    std::get<vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR>(devChain).fragmentShaderBarycentric = VK_TRUE;
    std::get<vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR>(devChain).rayTracingPipelineTraceRaysIndirect2 = VK_TRUE;
    std::get<vk::PhysicalDeviceHostQueryResetFeatures>(devChain).hostQueryReset = VK_TRUE;
    std::get<vk::PhysicalDeviceRayQueryFeaturesKHR>(devChain).rayQuery = VK_TRUE;

    auto& vmm = std::get<vk::PhysicalDeviceVulkanMemoryModelFeatures>(devChain);
    vmm.vulkanMemoryModel = VK_TRUE;
    vmm.vulkanMemoryModelDeviceScope = VK_TRUE;
    vmm.vulkanMemoryModelAvailabilityVisibilityChains = VK_TRUE;

    std::get<vk::PhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR>(devChain).shaderRelaxedExtendedInstruction = VK_TRUE;

    vk::Device device = phys.createDevice(devChain.get());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);
    vk::Queue gfxQueue = device.getQueue(gfxIndex, 0);

    // VMA allocator (align with mi/rhi/vk/vk_rhi.cpp)
    vma::Allocator vmaAllocator{};
    bool vmaAllocatorValid = false;
    if (kEnableVma) {
        vmaAllocator = vma::createAllocator(vma::AllocatorCreateInfo{
                vma::AllocatorCreateFlagBits::eKhrDedicatedAllocation // Vulkan 1.1
                | vma::AllocatorCreateFlagBits::eBufferDeviceAddress // Vulkan 1.2
                | vma::AllocatorCreateFlagBits::eKhrBindMemory2   // Vulkan 1.1
                | vma::AllocatorCreateFlagBits::eKhrMaintenance4, // Vulkan 1.3
                phys,
                device,
                256 * 1024 * 1024,
                nullptr, // No allocation callback
                nullptr, // No device memory callback
                nullptr, // No heap size limit
                nullptr, // Use default vulkan dispatcher for vulkan function calls
                instance,
                VK_MAKE_API_VERSION(0, 1, 3, 201)
        });
        vmaAllocatorValid = true;
    }

    // Swapchainvk
    auto caps = phys.getSurfaceCapabilitiesKHR(surface);
    auto formats = phys.getSurfaceFormatsKHR(surface);
    vk::SurfaceFormatKHR sfmt = formats[0];
    vk::Extent2D extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : vk::Extent2D{WIDTH, HEIGHT};

    vk::SwapchainCreateInfoKHR sci{};
    sci.surface = surface;
    sci.minImageCount = std::max(2u, caps.minImageCount);
    sci.imageFormat = sfmt.format;
    sci.imageColorSpace = sfmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eColorAttachment;
    sci.imageSharingMode = vk::SharingMode::eExclusive;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    sci.presentMode = vk::PresentModeKHR::eFifo;

    vk::SwapchainKHR swapchain = device.createSwapchainKHR(sci);

    // Enumerate swapchain images (needed for per-image semaphores + copy)
    auto swapchainImages = device.getSwapchainImagesKHR(swapchain);
    if (swapchainImages.empty()) {
        std::cerr << "No swapchain images\n";
        return 1;
    }

    // Track swapchain image layouts so we always transition from the correct oldLayout.
    // After vkAcquireNextImageKHR the image is typically in PRESENT_SRC_KHR (unless first use),
    // and validation will complain if we always claim oldLayout=UNDEFINED.
    std::vector<vk::ImageLayout> swapchainLayouts(swapchainImages.size(), vk::ImageLayout::eUndefined);

    vk::SemaphoreCreateInfo semInfo{};

    // Per-swapchain-image semaphores (align with vk_swapchain_*_semaphores_)
    std::vector<vk::Semaphore> swapchainImageAvailableSems;
    std::vector<vk::Semaphore> swapchainRenderFinishedSems;
    swapchainImageAvailableSems.resize(swapchainImages.size());
    swapchainRenderFinishedSems.resize(swapchainImages.size());
    for (size_t i = 0; i < swapchainImages.size(); ++i) {
        swapchainImageAvailableSems[i] = device.createSemaphore(semInfo);
        swapchainRenderFinishedSems[i] = device.createSemaphore(semInfo);
    }

    // Per-frame sync: still keep a fence per frame-in-flight to throttle CPU like renderer does
    // (renderFinished semaphore now becomes per-swapchain-image)
    vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};
    struct FrameSync {
        vk::Fence inFlight{};
    };
    std::array<FrameSync, kFramesInFlight> frames{};
    for (auto& f : frames) {
        f.inFlight = device.createFence(fenceInfo);
    }

    // Per-frame command pools (renderer-style: reset pool each frame)
    std::array<vk::CommandPool, kFramesInFlight> cmdPools{};
    {
        vk::CommandPoolCreateInfo cpci{};
        cpci.queueFamilyIndex = gfxIndex;
        cpci.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            cmdPools[i] = device.createCommandPool(cpci);
        }
    }

    // Create a lightweight backbuffer image (double buffered) like VulkanRHI::rhi_backbuffer_textures[2]
    vk::Image backbufferImages[2]{};
    vk::DeviceMemory backbufferMems[2]{};
    vk::ImageLayout backbufferLayouts[2]{vk::ImageLayout::eUndefined, vk::ImageLayout::eUndefined};

    auto findMemoryType = [&](uint32_t typeBits, vk::MemoryPropertyFlags flags) -> uint32_t {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) && ((memProps.memoryTypes[i].propertyFlags & flags) == flags)) {
                return i;
            }
        }
        return UINT32_MAX;
    };

    if (kEnableFrameEndCopyToSwapchain) {
        vk::ImageCreateInfo ici{};
        ici.imageType = vk::ImageType::e2D;
        ici.format = sfmt.format;
        ici.extent = vk::Extent3D{extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = vk::SampleCountFlagBits::e1;
        ici.tiling = vk::ImageTiling::eOptimal;
        // 需要 transfer src（拷到 swapchain），以及 color attachment 兼容（更像 backbuffer）
        ici.usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eColorAttachment;
        ici.sharingMode = vk::SharingMode::eExclusive;
        ici.initialLayout = vk::ImageLayout::eUndefined;

        for (int i = 0; i < 2; ++i) {
            backbufferImages[i] = device.createImage(ici);
            auto req = device.getImageMemoryRequirements(backbufferImages[i]);
            uint32_t memType = findMemoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
            if (memType == UINT32_MAX) {
                std::cerr << "No device local memory type for backbuffer\n";
                return 1;
            }
            backbufferMems[i] = device.allocateMemory(vk::MemoryAllocateInfo{req.size, memType});
            device.bindImageMemory(backbufferImages[i], backbufferMems[i], 0);
        }
    }

    uint64_t frameCounter = 0;

    // VMA per-frame allocations state
    struct VmaObject {
        vk::Buffer buffer{};
        vma::Allocation alloc{};
    };
    std::vector<VmaObject> vmaLiveObjects;
    vmaLiveObjects.reserve(kFramesInFlight * kVmaBuffersPerFrame);

    // -------------------- Descriptor pool + layout (for per-frame alloc/update) --------------------
    // Align with dump: descriptor set has 3 bindings; we CopyDescriptorSet 3 times each frame.
    vk::DescriptorSetLayout descSetLayout{};
    {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};

        bindings[0].binding = 0;
        bindings[0].descriptorCount = kBindlessBinding0Count;
        bindings[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[0].stageFlags = vk::ShaderStageFlagBits::eAll;

        bindings[1].binding = 1;
        bindings[1].descriptorCount = kBindlessBinding1Count;
        bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[1].stageFlags = vk::ShaderStageFlagBits::eAll;

        bindings[2].binding = 2;
        bindings[2].descriptorCount = kBindlessBinding2Count;
        bindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[2].stageFlags = vk::ShaderStageFlagBits::eAll;

        vk::DescriptorSetLayoutCreateInfo dslci{};
        dslci.bindingCount = (uint32_t)bindings.size();
        dslci.pBindings = bindings.data();
        descSetLayout = device.createDescriptorSetLayout(dslci);
    }

    vk::DescriptorPool descriptorPool{};
    if (kEnablePerFrameDescriptorPoolReset || kEnableBindlessLikeCopyPerFrame) {
        // 对齐 vk_cmd_exec_common.cpp 的思路：每帧 reset pool，所以 pool 要足够容纳一帧峰值。
        vk::DescriptorPoolSize poolSize{};
        poolSize.type = vk::DescriptorType::eStorageBuffer;
        poolSize.descriptorCount = kDescriptorPoolMaxStorageBufferDescsPerFrame;

        vk::DescriptorPoolCreateInfo dpci{};
        dpci.maxSets = kDescriptorPoolMaxSetsPerFrame;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        descriptorPool = device.createDescriptorPool(dpci);
    }

    // IMPORTANT: do NOT allocate long-lived (across frames) descriptor sets from a pool that gets reset.
    // Pool reset invalidates *all* sets allocated from that pool, which will trip validation at the next
    // vkUpdateDescriptorSets(CopyDescriptorSet) using those sets.
    vk::DescriptorPool bindlessDescriptorPool{};
    if (kEnableBindlessLikeCopyPerFrame) {
        // We only need 2 sets, but ensure the pool has enough descriptors for those sets.
        const uint32_t perSetDescCount = (kBindlessBinding0Count + kBindlessBinding1Count + kBindlessBinding2Count);

        vk::DescriptorPoolSize poolSize{};
        poolSize.type = vk::DescriptorType::eStorageBuffer;
        poolSize.descriptorCount = perSetDescCount * 2u;

        vk::DescriptorPoolCreateInfo dpci{};
        dpci.maxSets = 2;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;

        bindlessDescriptorPool = device.createDescriptorPool(dpci);
    }

    // 用于 update 的 buffer（随便绑一个，避免 update 写 nullptr 引发 validation 噪音）
    // 这里用 host visible staging buffer，简化内存选择。
    vk::Buffer dummyBuf{};
    vk::DeviceMemory dummyMem{};
    {
        vk::BufferCreateInfo bi{};
        bi.size = 256;
        bi.usage = vk::BufferUsageFlagBits::eStorageBuffer;
        dummyBuf = device.createBuffer(bi);
        auto req = device.getBufferMemoryRequirements(dummyBuf);
        uint32_t hostType = UINT32_MAX;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                ((memProps.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostVisible) != vk::MemoryPropertyFlags{})) {
                hostType = i;
                break;
            }
        }
        if (hostType == UINT32_MAX) {
            std::cerr << "No host visible memory for dummy buffer\n";
            return 1;
        }
        dummyMem = device.allocateMemory(vk::MemoryAllocateInfo{req.size, hostType});
        device.bindBufferMemory(dummyBuf, dummyMem, 0);
    }

    // bindless-like: 两套 descriptor set，每帧做 CopyDescriptorSet + null writes
    vk::DescriptorSet bindlessSets[2]{};
    if (kEnableBindlessLikeCopyPerFrame) {
        std::array<vk::DescriptorSetLayout, 2> layouts{descSetLayout, descSetLayout};
        auto sets = device.allocateDescriptorSets(
            vk::DescriptorSetAllocateInfo{bindlessDescriptorPool, (uint32_t)layouts.size(), layouts.data()});
        bindlessSets[0] = sets[0];
        bindlessSets[1] = sets[1];

        // Initialize set0: fill all descriptors with dummyBuf
        // binding 0
        {
            std::vector<vk::WriteDescriptorSet> writes;
            std::vector<vk::DescriptorBufferInfo> infos;
            writes.reserve(kBindlessBinding0Count + kBindlessBinding1Count + kBindlessBinding2Count);
            infos.reserve(kBindlessBinding0Count + kBindlessBinding1Count + kBindlessBinding2Count);

            auto pushWrite = [&](vk::DescriptorSet set, uint32_t binding, uint32_t arrayElement) {
                infos.push_back(vk::DescriptorBufferInfo{dummyBuf, 0, VK_WHOLE_SIZE});
                vk::WriteDescriptorSet w{};
                w.dstSet = set;
                w.dstBinding = binding;
                w.dstArrayElement = arrayElement;
                w.descriptorCount = 1;
                w.descriptorType = vk::DescriptorType::eStorageBuffer;
                w.pBufferInfo = &infos.back();
                writes.push_back(w);
            };

            for (uint32_t i = 0; i < kBindlessBinding0Count; ++i) pushWrite(bindlessSets[0], 0, i);
            for (uint32_t i = 0; i < kBindlessBinding1Count; ++i) pushWrite(bindlessSets[0], 1, i);
            for (uint32_t i = 0; i < kBindlessBinding2Count; ++i) pushWrite(bindlessSets[0], 2, i);

            device.updateDescriptorSets(writes, {});
        }
    }

    // C: timestamp query pool
    vk::QueryPool timestampPool{};
    if (kEnableTimestampQuery) {
        vk::QueryPoolCreateInfo qpci{};
        qpci.queryType = vk::QueryType::eTimestamp;
        qpci.queryCount = kMaxNumTimestampQueries;
        timestampPool = device.createQueryPool(qpci);
        // 初始 reset 一次
        device.resetQueryPool(timestampPool, 0, kMaxNumTimestampQueries);
    }

    // 生成一个固定的“每帧要 free 的 slot 列表”（模拟 PrepareDelayedSlotsForRHIFree 的 batched slots）
    std::vector<uint32_t> bindlessSlotsToFree;
    bindlessSlotsToFree.resize(kBindlessFreeSlotsPerFrame);
    std::iota(bindlessSlotsToFree.begin(), bindlessSlotsToFree.end(), 0u);

    uint32_t bindlessSetIndex = 0;


    vk::CommandBuffer cmds [kFramesInFlight]{};

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const uint32_t frameIndex = static_cast<uint32_t>(frameCounter % kFramesInFlight);
        auto& f = frames[frameIndex];

        // Match dump: wait fence timeout is 500ms; then reset fence
        (void)device.waitForFences(1, &f.inFlight, VK_TRUE, 500000000ull);
        device.resetFences(1, &f.inFlight);

        // Match dump ordering: reset cmd pool + reset descriptor pool (Thread 1)
        // IMPORTANT: resetCommandPool must happen *after* we know the previous submit using this pool has finished.
        if (kResetCommandPoolPerFrame) {
            (void)device.resetCommandPool(cmdPools[frameIndex], {});
            cmds[frameIndex] = device.allocateCommandBuffers(
                vk::CommandBufferAllocateInfo{
                    cmdPools[frameIndex],
                    vk::CommandBufferLevel::ePrimary,
                    1
                }
            ).front();
        } else {
            // 使用 free - allocate 代替pool reset
            device.freeCommandBuffers(
                cmdPools[frameIndex],
                cmds[frameIndex]
            );
            cmds[frameIndex] = device.allocateCommandBuffers(
                vk::CommandBufferAllocateInfo{
                    cmdPools[frameIndex],
                    vk::CommandBufferLevel::ePrimary,
                    1
                }
            ).front();
        }
        if (kEnablePerFrameDescriptorPoolReset) {
            (void)device.resetDescriptorPool(descriptorPool);
        }

        // Allocate+begin command buffer before acquire (as in dump)
        vk::CommandBuffer cmd = cmds[frameIndex];
        cmd.begin(vk::CommandBufferBeginInfo{});

        // Dynamic states seen in dump
        cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
        cmd.setCullMode(vk::CullModeFlagBits::eNone);
        cmd.setDepthBiasEnable(VK_FALSE);
        cmd.setPolygonModeEXT(vk::PolygonMode::eFill);

        // Acquire after begin (Thread 1) with 10s timeout
        uint32_t swapchainImageIndex = 0;
        vk::Semaphore imageReadySem = swapchainImageAvailableSems[frameIndex % swapchainImages.size()];
        auto acquireRes = device.acquireNextImageKHR(swapchain, 10000000000ull, imageReadySem, {}, &swapchainImageIndex);
        if (acquireRes == vk::Result::eErrorOutOfDateKHR) {
            cmd.end();
            if (!kResetCommandPoolPerFrame) {
                device.freeCommandBuffers(cmdPools[frameIndex], {cmd});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (acquireRes != vk::Result::eSuccess && acquireRes != vk::Result::eSuboptimalKHR) {
            cmd.end();
            if (!kResetCommandPoolPerFrame) {
                device.freeCommandBuffers(cmdPools[frameIndex], {cmd});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // Match dump: vkUpdateDescriptorSets: descriptorWriteCount=0, descriptorCopyCount=3
        if (kEnableBindlessLikeCopyPerFrame) {
            bindlessSetIndex = (bindlessSetIndex + 1) % 2;

            std::array<vk::CopyDescriptorSet, 3> copies{};
            copies[0].srcSet = bindlessSets[bindlessSetIndex ^ 1];
            copies[0].srcBinding = 0;
            copies[0].srcArrayElement = 0;
            copies[0].dstSet = bindlessSets[bindlessSetIndex];
            copies[0].dstBinding = 0;
            copies[0].dstArrayElement = 0;
            copies[0].descriptorCount = kBindlessBinding0Count;

            copies[1].srcSet = bindlessSets[bindlessSetIndex ^ 1];
            copies[1].srcBinding = 1;
            copies[1].srcArrayElement = 0;
            copies[1].dstSet = bindlessSets[bindlessSetIndex];
            copies[1].dstBinding = 1;
            copies[1].dstArrayElement = 0;
            copies[1].descriptorCount = kBindlessBinding1Count;

            copies[2].srcSet = bindlessSets[bindlessSetIndex ^ 1];
            copies[2].srcBinding = 2;
            copies[2].srcArrayElement = 0;
            copies[2].dstSet = bindlessSets[bindlessSetIndex];
            copies[2].dstBinding = 2;
            copies[2].dstArrayElement = 0;
            copies[2].descriptorCount = kBindlessBinding2Count;

            device.updateDescriptorSets({}, copies);
        }

        // Timestamp/query (optional): keep but note it will deviate from the dump if enabled
        if (kEnableTimestampQuery) {
            device.resetQueryPool(timestampPool, 0, kMaxNumTimestampQueries);
            const uint32_t base = (frameIndex % kFramesInFlight) * kTimestampQueriesPerFrame;
            const uint32_t count = std::min(kTimestampsToWritePerFrame, kTimestampQueriesPerFrame);
            for (uint32_t i = 0; i < count; ++i) {
                cmd.writeTimestamp(vk::PipelineStageFlagBits::eAllCommands, timestampPool, base + i);
            }
        }

        vk::Semaphore presentReadySem = swapchainRenderFinishedSems[swapchainImageIndex];

        if (kEnableFrameEndCopyToSwapchain) {
            const int bbIndex = static_cast<int>(frameCounter & 1ull);
            vk::Image backbuffer = backbufferImages[bbIndex];

            vk::ImageMemoryBarrier barrierBB{};
            barrierBB.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            barrierBB.dstAccessMask = vk::AccessFlagBits::eTransferRead;
            barrierBB.oldLayout = backbufferLayouts[bbIndex];
            barrierBB.newLayout = vk::ImageLayout::eTransferSrcOptimal;
            barrierBB.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrierBB.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrierBB.image = backbuffer;
            barrierBB.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

            vk::ImageMemoryBarrier barrierSC{};
            barrierSC.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            barrierSC.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrierSC.oldLayout = swapchainLayouts[swapchainImageIndex];
            barrierSC.newLayout = vk::ImageLayout::eTransferDstOptimal;
            barrierSC.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrierSC.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrierSC.image = swapchainImages[swapchainImageIndex];
            barrierSC.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

            std::array<vk::ImageMemoryBarrier, 2> barriers{barrierBB, barrierSC};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                                {}, {}, {}, barriers);
            backbufferLayouts[bbIndex] = vk::ImageLayout::eTransferSrcOptimal;
            swapchainLayouts[swapchainImageIndex] = vk::ImageLayout::eTransferDstOptimal;

            vk::ImageCopy copyRegion{};
            copyRegion.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copyRegion.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copyRegion.extent = vk::Extent3D{extent.width, extent.height, 1};
            cmd.copyImage(backbuffer, vk::ImageLayout::eTransferSrcOptimal,
                          swapchainImages[swapchainImageIndex], vk::ImageLayout::eTransferDstOptimal,
                          copyRegion);

            vk::ImageMemoryBarrier barrierPresent{};
            barrierPresent.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            barrierPresent.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
            barrierPresent.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            barrierPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
            barrierPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrierPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrierPresent.image = swapchainImages[swapchainImageIndex];
            barrierPresent.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                {}, {}, {}, barrierPresent);
            swapchainLayouts[swapchainImageIndex] = vk::ImageLayout::ePresentSrcKHR;

            if (kEnableExecutionBarrier) {
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eNone,
                                    {}, {}, {}, {});
            }

            if (kEnableMegaBarrier) {
                cmd.pipelineBarrier(
                    vk::PipelineStageFlagBits::eAllGraphics,
                    vk::PipelineStageFlagBits::eAllGraphics,
                    {},
                    vk::MemoryBarrier{
                        vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                        vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
                    },
                    {},
                    {}
                );
            }
        }

        cmd.end();

        vk::PipelineStageFlags waitStages = vk::PipelineStageFlagBits::eTransfer;

        vk::SubmitInfo submitInfo{};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &imageReadySem;
        submitInfo.pWaitDstStageMask = &waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &presentReadySem;

        gfxQueue.submit(1, &submitInfo, f.inFlight);

        vk::Result presentResult;
        vk::PresentInfoKHR presentInfo{};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &presentReadySem;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &swapchainImageIndex;
        presentInfo.pResults = &presentResult;
        (void)gfxQueue.presentKHR(presentInfo);

        // VMA traffic: alloc/free a bunch of small buffers every frame
        if (kEnableVma && kVmaAllocFreePerFrame && vmaAllocatorValid) {
            for (auto& o : vmaLiveObjects) {
                if (o.buffer) vmaAllocator.destroyBuffer(o.buffer, o.alloc);
            }
            vmaLiveObjects.clear();

            for (uint32_t i = 0; i < kVmaBuffersPerFrame; ++i) {
                vk::BufferCreateInfo binfo{};
                binfo.size = kVmaBufferSize;
                binfo.usage = vk::BufferUsageFlagBits::eStorageBuffer
                            | vk::BufferUsageFlagBits::eTransferSrc
                            | vk::BufferUsageFlagBits::eTransferDst
                            | vk::BufferUsageFlagBits::eShaderDeviceAddress;

                vma::AllocationCreateInfo ainfo{};
                ainfo.usage = vma::MemoryUsage::eAuto;

                if (kVmaUseDedicatedSometimes && (i % kVmaDedicatedModulo) == 0) {
                    ainfo.flags |= vma::AllocationCreateFlagBits::eDedicatedMemory;
                }

                auto created = vmaAllocator.createBuffer(binfo, ainfo);
                VmaObject obj{};
                obj.buffer = created.first;
                obj.alloc = created.second;
                vmaLiveObjects.push_back(obj);
            }
        }

        ++frameCounter;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    device.waitIdle();

    // VMA cleanup
    if (kEnableVma && vmaAllocatorValid) {
        for (auto& o : vmaLiveObjects) {
            if (o.buffer) vmaAllocator.destroyBuffer(o.buffer, o.alloc);
        }
        vmaLiveObjects.clear();
        vmaAllocator.destroy();
        vmaAllocatorValid = false;
    }

    // Cleanup: destroy per-swapchain semaphores + backbuffers
    for (auto s : swapchainImageAvailableSems) device.destroySemaphore(s);
    for (auto s : swapchainRenderFinishedSems) device.destroySemaphore(s);

    if (kEnableFrameEndCopyToSwapchain) {
        for (int i = 0; i < 2; ++i) {
            device.destroyImage(backbufferImages[i]);
            device.freeMemory(backbufferMems[i]);
        }
    }

    // Destroy descriptor-related resources before destroying the device
    if (kEnablePerFrameDescriptorPoolReset || kEnableBindlessLikeCopyPerFrame) {
        device.destroyDescriptorPool(descriptorPool);
        device.destroyDescriptorSetLayout(descSetLayout);
    }

    if (kEnableBindlessLikeCopyPerFrame) {
        device.destroyDescriptorPool(bindlessDescriptorPool);
    }

    device.destroyBuffer(dummyBuf);
    device.freeMemory(dummyMem);

    if (kEnableTimestampQuery) {
        device.destroyQueryPool(timestampPool);
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        device.destroyCommandPool(cmdPools[i]);
    }

    for (auto& f : frames) {
        device.destroyFence(f.inFlight);
    }

    device.destroySwapchainKHR(swapchain);
    instance.destroySurfaceKHR(surface);
    device.destroy();
    instance.destroy();


    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
