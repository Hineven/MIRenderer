#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>

// 这一行会让 new 包含文件名和行号信息
#ifdef _DEBUG
    #define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cstring>
#include <array>
#include <thread>
#include <chrono>


VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static const uint32_t WIDTH = 800;
static const uint32_t HEIGHT = 600;

#define VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT


int main() {
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    char * ptr = new char[1024 * 1024 * 12]; // Allocate 12 MB to test memory leaking detection
    ptr[0] = '1'; // Use the memory to avoid optimization
    ptr[1] = '\0';
    printf("%s", ptr);
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(WIDTH, HEIGHT, "MinCaptureTest", nullptr, nullptr);
    if (!window) { std::cerr << "Failed to create window\n"; return 1; }

    // Create Vulkan instance
    vk::ApplicationInfo appInfo{"MinCaptureTest", VK_MAKE_VERSION(1,0,0), "", VK_MAKE_VERSION(1,0,0), VK_API_VERSION_1_3};
    std::vector<const char*> instExts;
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    for (uint32_t i = 0; i < glfwExtCount; ++i) instExts.push_back(glfwExts[i]);
#ifndef NDEBUG
    instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    vk::InstanceCreateInfo instInfo({}, &appInfo, 0, nullptr, (uint32_t)instExts.size(), instExts.data());
    // Initialize global dispatcher using vkGetInstanceProcAddr, then create instance and initialize again
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
    vk::Instance instance = vk::createInstance(instInfo);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

    // Pick physical device (first discrete)
    auto devices = instance.enumeratePhysicalDevices();
    if (devices.empty()) { std::cerr << "No physical device\n"; return 1; }
    vk::PhysicalDevice phys = devices[0];
    for (auto d : devices) {
        if (d.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) { phys = d; break; }
    }

    // Create surface
    VkSurfaceKHR c_surface;
    if (glfwCreateWindowSurface(instance, window, nullptr, &c_surface) != VK_SUCCESS) {
        std::cerr << "Failed to create surface\n"; return 1;
    }
    vk::SurfaceKHR surface{c_surface};

    // Choose queue family
    auto qprops = phys.getQueueFamilyProperties();
    uint32_t gfxIndex = UINT32_MAX;
    for (uint32_t i = 0; i < qprops.size(); ++i) {
        if (qprops[i].queueFlags & vk::QueueFlagBits::eGraphics) { gfxIndex = i; break; }
    }
    if (gfxIndex == UINT32_MAX) { std::cerr << "No graphics queue\n"; return 1; }

    // Device extensions
    std::vector<const char*> devExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
    };

    // Features chain
    vk::PhysicalDeviceFeatures feats{};
    vk::StructureChain<vk::DeviceCreateInfo,
        vk::PhysicalDeviceAccelerationStructureFeaturesKHR,
        vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
        vk::PhysicalDeviceBufferDeviceAddressFeatures
    > devChain;
    auto & dci = std::get<0>(devChain);
    dci.setPEnabledFeatures(&feats);
    std::array<float,1> priorities{1.0f};
    vk::DeviceQueueCreateInfo qci{};
    qci.queueFamilyIndex = gfxIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = priorities.data();
    std::vector<vk::DeviceQueueCreateInfo> qcis{qci};
    dci.setQueueCreateInfos(qcis);
    dci.setPEnabledExtensionNames(devExts);
    auto & asFeat = std::get<vk::PhysicalDeviceAccelerationStructureFeaturesKHR>(devChain);
    auto & rtFeat = std::get<vk::PhysicalDeviceRayTracingPipelineFeaturesKHR>(devChain);
    auto & bdaFeat = std::get<vk::PhysicalDeviceBufferDeviceAddressFeatures>(devChain);
    asFeat.accelerationStructure = VK_TRUE;
    asFeat.accelerationStructureCaptureReplay = VK_TRUE;
    bdaFeat.bufferDeviceAddress = VK_TRUE;
    // Many SDKs expose capture-replay under the same struct
    bdaFeat.bufferDeviceAddressCaptureReplay = VK_TRUE;
    rtFeat.rayTracingPipeline = VK_TRUE;

    vk::Device device = phys.createDevice(devChain.get());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    // Swapchain minimal setup
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
    // Get graphics queue for present
    vk::Queue gfxQueue = device.getQueue(gfxIndex, 0);
    // Create semaphores for acquire/present sync
    vk::SemaphoreCreateInfo semInfo{};
    vk::Semaphore imageAvailable = device.createSemaphore(semInfo);
    vk::Semaphore renderFinished = device.createSemaphore(semInfo);

    // Create a minimal empty acceleration structure buffer and AS with capture/replay flags
    const vk::DeviceSize asSize = 1024; // small size
    vk::BufferCreateInfo bci{};
    bci.size = asSize;
    bci.usage = vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress;
#ifdef VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT
    bci.flags |= vk::BufferCreateFlagBits::eDeviceAddressCaptureReplay;
#endif
    vk::Buffer asBuffer = device.createBuffer(bci);
    auto memReqs = device.getBufferMemoryRequirements(asBuffer);

    // Find device-local memory type
    auto memProps = phys.getMemoryProperties();
    uint32_t typeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) { typeIdx = i; break; }
    }
    if (typeIdx == UINT32_MAX) { std::cerr << "No device-local memory type for AS buffer\n"; return 1; }

    vk::MemoryAllocateFlagsInfo maf{};
#ifdef VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT
    maf.flags = vk::MemoryAllocateFlagBits::eDeviceAddressCaptureReplay;
#endif
    vk::MemoryAllocateInfo mai{};
    mai.allocationSize = memReqs.size;
    mai.memoryTypeIndex = typeIdx;
    mai.pNext = &maf;

    vk::DeviceMemory asMem = device.allocateMemory(mai);
    device.bindBufferMemory(asBuffer, asMem, 0);

    vk::AccelerationStructureCreateInfoKHR asci{};
    asci.buffer = asBuffer;
    asci.size = asSize;
    asci.type = vk::AccelerationStructureTypeKHR::eTopLevel;
#ifdef VK_ACCELERATION_STRUCTURE_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR
    asci.flags |= vk::AccelerationStructureCreateFlagBitsKHR::eDeviceAddressCaptureReplayKHR;
#endif
    vk::AccelerationStructureKHR as = device.createAccelerationStructureKHR(asci);

    vk::AccelerationStructureDeviceAddressInfoKHR addrInfo{};
    addrInfo.accelerationStructure = as;
    uint64_t addr = device.getAccelerationStructureAddressKHR(addrInfo);
    std::cout << "AS device address: 0x" << std::hex << addr << std::dec << "\n";

    // Main loop: acquire and present swapchain image to enable Nsight capture
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        // Acquire next image
        uint32_t imageIndex = 0;
        auto acquireRes = device.acquireNextImageKHR(swapchain, UINT64_MAX, imageAvailable, {});
        if (acquireRes.result == vk::Result::eSuccess || acquireRes.result == vk::Result::eSuboptimalKHR) {
            imageIndex = acquireRes.value;
            // For this minimal app we don't submit any command buffers; just signal renderFinished immediately.
            // In a real app, you'd submit a queue with work and signal renderFinished when done.
            // Present
            vk::PresentInfoKHR presentInfo{};
            presentInfo.waitSemaphoreCount = 1;
            vk::Semaphore waitSems[1] = { imageAvailable };
            presentInfo.pWaitSemaphores = waitSems;
            presentInfo.swapchainCount = 1;
            vk::SwapchainKHR swapchains[1] = { swapchain };
            presentInfo.pSwapchains = swapchains;
            uint32_t indices[1] = { imageIndex };
            presentInfo.pImageIndices = indices;
            (void)gfxQueue.presentKHR(presentInfo);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Cleanup
    device.destroySemaphore(renderFinished);
    device.destroySemaphore(imageAvailable);
    device.destroyAccelerationStructureKHR(as);
    device.destroyBuffer(asBuffer);
    device.freeMemory(asMem);
    device.destroySwapchainKHR(swapchain);
    instance.destroySurfaceKHR(surface);
    device.destroy();
    instance.destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    _CrtDumpMemoryLeaks();

    return 0;
}
