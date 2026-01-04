// Minimal Vulkan ray tracing repro / stress case.
//
// This intentionally does the following every frame:
//   vkCmdBindPipeline (ray tracing)
//   vkAllocateDescriptorSets
//   vkUpdateDescriptorSets
//   vkCmdBindDescriptorSets
//   vkCmdTraceRaysIndirectKHR
//
// It renders nothing meaningful; it exists to isolate driver behavior.

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>
#include <algorithm>
#include <cstdio>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static constexpr uint32_t kWidth = 640;
static constexpr uint32_t kHeight = 360;
static constexpr uint32_t kFramesInFlight = 2;

static constexpr bool kResetCommandPoolPerFrame = false;
static constexpr bool kFreeCommandBufferPerFrame = true;
static constexpr bool kResetDescriptorPoolPerFrame = true;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    [[nodiscard]] bool complete() const { return graphics.has_value() && present.has_value(); }
};

static QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice phys, vk::SurfaceKHR surface) {
    QueueFamilyIndices out{};
    auto props = phys.getQueueFamilyProperties();
    for (uint32_t i = 0; i < static_cast<uint32_t>(props.size()); ++i) {
        if ((props[i].queueFlags & vk::QueueFlagBits::eGraphics) && !out.graphics.has_value()) out.graphics = i;
        if (!out.present.has_value()) {
            VkBool32 supported = VK_FALSE;
            (void)phys.getSurfaceSupportKHR(i, surface, &supported);
            if (supported) out.present = i;
        }
        if (out.complete()) break;
    }
    return out;
}

static vk::SurfaceFormatKHR chooseSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == vk::Format::eB8G8R8A8Srgb && f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) return f;
    }
    return formats.empty() ? vk::SurfaceFormatKHR{} : formats[0];
}

static vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& modes) {
    for (auto m : modes) {
        if (m == vk::PresentModeKHR::eMailbox) return m;
    }
    return vk::PresentModeKHR::eFifo;
}

static vk::Extent2D chooseExtent(const vk::SurfaceCapabilitiesKHR& caps, GLFWwindow* window) {
    if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    vk::Extent2D e{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    e.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, e.width));
    e.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, e.height));
    return e;
}

static uint32_t findMemoryType(vk::PhysicalDevice phys, uint32_t typeBits, vk::MemoryPropertyFlags props) {
    auto memProps = phys.getMemoryProperties();
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && ((memProps.memoryTypes[i].propertyFlags & props) == props)) return i;
    }
    return UINT32_MAX;
}

struct Buffer {
    vk::Buffer buf{};
    vk::DeviceMemory mem{};
};

static Buffer createBuffer(vk::PhysicalDevice phys, vk::Device dev, vk::DeviceSize size,
                           vk::BufferUsageFlags usage, vk::MemoryPropertyFlags memProps,
                           bool deviceAddress) {
    vk::BufferCreateInfo bci{};
    bci.size = size;
    bci.usage = usage | (deviceAddress ? vk::BufferUsageFlagBits::eShaderDeviceAddress : vk::BufferUsageFlags{});
    bci.sharingMode = vk::SharingMode::eExclusive;

    vk::Buffer buf = dev.createBuffer(bci);
    auto req = dev.getBufferMemoryRequirements(buf);

    vk::MemoryAllocateFlagsInfo flagsInfo{};
    flagsInfo.flags = deviceAddress ? vk::MemoryAllocateFlagBits::eDeviceAddress : vk::MemoryAllocateFlags{};

    vk::MemoryAllocateInfo mai{};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, memProps);
    if (deviceAddress) mai.pNext = &flagsInfo;

    vk::DeviceMemory mem = dev.allocateMemory(mai);
    dev.bindBufferMemory(buf, mem, 0);

    return {buf, mem};
}

struct Image {
    vk::Image img{};
    vk::DeviceMemory mem{};
    vk::ImageView view{};
};

static Image createStorageImage(vk::PhysicalDevice phys, vk::Device dev, vk::Format fmt, uint32_t w, uint32_t h) {
    vk::ImageCreateInfo ici{};
    ici.imageType = vk::ImageType::e2D;
    ici.format = fmt;
    ici.extent = vk::Extent3D{w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = vk::SampleCountFlagBits::e1;
    ici.tiling = vk::ImageTiling::eOptimal;
    ici.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst;
    ici.sharingMode = vk::SharingMode::eExclusive;
    ici.initialLayout = vk::ImageLayout::eUndefined;

    vk::Image img = dev.createImage(ici);
    auto req = dev.getImageMemoryRequirements(img);

    vk::MemoryAllocateInfo mai{};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::DeviceMemory mem = dev.allocateMemory(mai);
    dev.bindImageMemory(img, mem, 0);

    vk::ImageViewCreateInfo vci{};
    vci.image = img;
    vci.viewType = vk::ImageViewType::e2D;
    vci.format = fmt;
    vci.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::ImageView view = dev.createImageView(vci);

    return {img, mem, view};
}

static void transitionImage(vk::CommandBuffer cmd, vk::Image img, vk::ImageLayout oldL, vk::ImageLayout newL) {
    vk::ImageMemoryBarrier b{};
    b.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    b.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, b);
}

int main() {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();

    if (!glfwInit()) {
        std::cerr << "glfwInit failed\n";
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "min_rt_leak_case", nullptr, nullptr);
    if (!window) {
        std::cerr << "glfwCreateWindow failed\n";
        glfwTerminate();
        return 1;
    }

    // Instance
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> instExts;
    instExts.reserve(glfwExtCount + 4);
    for (uint32_t i = 0; i < glfwExtCount; ++i) instExts.push_back(glfwExts[i]);
#ifndef NDEBUG
    instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "min_rt_leak_case";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    vk::InstanceCreateInfo ici{};
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();

    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
    vk::Instance instance = vk::createInstance(ici);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

    VkSurfaceKHR rawSurface{};
    if (glfwCreateWindowSurface(instance, window, nullptr, &rawSurface) != VK_SUCCESS) {
        std::cerr << "glfwCreateWindowSurface failed\n";
        return 1;
    }
    vk::SurfaceKHR surface(rawSurface);

    // Pick device
    auto physDevices = instance.enumeratePhysicalDevices();
    if (physDevices.empty()) {
        std::cerr << "No Vulkan physical devices found\n";
        return 1;
    }

    vk::PhysicalDevice phys = physDevices[0];
    for (auto d : physDevices) {
        if (d.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) { phys = d; break; }
    }

    // Check RT extensions
    auto availExts = phys.enumerateDeviceExtensionProperties();
    auto hasExt = [&](const char* name) {
        for (auto& e : availExts) {
            if (strcmp(e.extensionName, name) == 0) return true;
        }
        return false;
    };

    if (!hasExt(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
        !hasExt(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ||
        !hasExt(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ||
        !hasExt(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) ||
        !hasExt(VK_KHR_SPIRV_1_4_EXTENSION_NAME) ||
        !hasExt(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME)) {
        std::cerr << "Missing required ray tracing extensions on this device\n";
        return 1;
    }

    QueueFamilyIndices q = findQueueFamilies(phys, surface);
    if (!q.complete()) {
        std::cerr << "No suitable queue families (graphics+present)\n";
        return 1;
    }

    float prio = 1.0f;
    std::vector<vk::DeviceQueueCreateInfo> qcis;
    std::vector<uint32_t> unique = {q.graphics.value(), q.present.value()};
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    for (uint32_t family : unique) {
        vk::DeviceQueueCreateInfo qci{};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        qcis.push_back(qci);
    }

    // Device features for RT
    vk::PhysicalDeviceBufferDeviceAddressFeatures bdaFeat{};
    bdaFeat.bufferDeviceAddress = VK_TRUE;

    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeFeat{};
    rtPipeFeat.rayTracingPipeline = VK_TRUE;
    rtPipeFeat.rayTracingPipelineTraceRaysIndirect = VK_TRUE;
    rtPipeFeat.pNext = &bdaFeat;

    vk::PhysicalDeviceAccelerationStructureFeaturesKHR asFeat{};
    asFeat.accelerationStructure = VK_TRUE;
    asFeat.pNext = &rtPipeFeat;

    vk::PhysicalDeviceRobustness2FeaturesKHR robustness2Feat{};
    robustness2Feat.nullDescriptor = VK_TRUE;
    robustness2Feat.pNext = &asFeat;

    std::vector<const char*> devExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };

    vk::DeviceCreateInfo dci{};
    dci.queueCreateInfoCount = (uint32_t)qcis.size();
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    dci.pNext = &robustness2Feat;

    vk::Device device = phys.createDevice(dci);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    vk::Queue graphicsQ = device.getQueue(q.graphics.value(), 0);
    vk::Queue presentQ = device.getQueue(q.present.value(), 0);

    // Swapchain (reusing main2.cpp pattern)
    auto caps = phys.getSurfaceCapabilitiesKHR(surface);
    auto formats = phys.getSurfaceFormatsKHR(surface);
    auto presentModes = phys.getSurfacePresentModesKHR(surface);
    vk::SurfaceFormatKHR surfFmt = chooseSurfaceFormat(formats);
    vk::PresentModeKHR presentMode = choosePresentMode(presentModes);
    vk::Extent2D extent = chooseExtent(caps, window);

    uint32_t imageCount = std::max(caps.minImageCount, kFramesInFlight + 1);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    vk::SwapchainCreateInfoKHR sci{};
    sci.surface = surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = surfFmt.format;
    sci.imageColorSpace = surfFmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;

    if (q.graphics.value() != q.present.value()) {
        std::array<uint32_t, 2> families = {q.graphics.value(), q.present.value()};
        sci.imageSharingMode = vk::SharingMode::eConcurrent;
        sci.queueFamilyIndexCount = (uint32_t)families.size();
        sci.pQueueFamilyIndices = families.data();
    } else {
        sci.imageSharingMode = vk::SharingMode::eExclusive;
    }

    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    sci.presentMode = presentMode;
    sci.clipped = VK_TRUE;

    vk::SwapchainKHR swapchain = device.createSwapchainKHR(sci);
    auto swapchainImages = device.getSwapchainImagesKHR(swapchain);
    std::vector<vk::ImageLayout> swapchainLayouts(swapchainImages.size(), vk::ImageLayout::eUndefined);

    // Per-image semaphores (same as main2)
    vk::FenceCreateInfo fenceCI{vk::FenceCreateFlagBits::eSignaled};
    vk::SemaphoreCreateInfo semCI{};

    std::array<vk::Semaphore, kFramesInFlight> imageAvailableSems{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) imageAvailableSems[i] = device.createSemaphore(semCI);

    std::vector<vk::Semaphore> renderFinishedSems(swapchainImages.size());
    for (size_t i = 0; i < swapchainImages.size(); ++i) renderFinishedSems[i] = device.createSemaphore(semCI);

    struct Frame {
        vk::CommandPool pool{};
        vk::DescriptorPool descPool{};
        vk::Fence fence{};
    };

    // Create small storage image the raygen writes to
    Image storageImg = createStorageImage(phys, device, vk::Format::eR8G8B8A8Unorm, 8, 8);

    // Create a dummy TLAS handle = VK_NULL_HANDLE (still bound as descriptor)
    // Some drivers accept VK_NULL_HANDLE in AS descriptors, others might validate.
    // If this fails on your system, we can add a real TLAS build in a follow-up.
    vk::AccelerationStructureKHR dummyTLAS = VK_NULL_HANDLE;

    // Descriptor set layout (set0)
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
    bindings[0] = vk::DescriptorSetLayoutBinding{}
        .setBinding(0)
        .setDescriptorType(vk::DescriptorType::eUniformBuffer)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eRaygenKHR);
    bindings[1] = vk::DescriptorSetLayoutBinding{}
        .setBinding(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eRaygenKHR);
    bindings[2] = vk::DescriptorSetLayoutBinding{}
        .setBinding(2)
        .setDescriptorType(vk::DescriptorType::eAccelerationStructureKHR)
        .setDescriptorCount(1)
        .setStageFlags(vk::ShaderStageFlagBits::eRaygenKHR);

    vk::DescriptorSetLayout set0Layout = device.createDescriptorSetLayout(
        vk::DescriptorSetLayoutCreateInfo{}.setBindings(bindings)
    );

    vk::PipelineLayout pipeLayout = device.createPipelineLayout(
        vk::PipelineLayoutCreateInfo{}.setSetLayouts(set0Layout)
    );

    // Ray tracing pipeline creation requires SPIR-V; this single-file repro intentionally keeps everything local.
    // To avoid depending on shaderc/glslang runtime on users' machine, we compile GLSL -> SPIR-V at *build time*
    // in this repo using pre-generated SPV files. If you see the exception below, it means the SPV files aren't
    // generated yet.

    auto loadSpv = [](const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) throw std::runtime_error(std::string("Failed to open ") + path);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint32_t> data((size_t)sz / 4);
        fread(data.data(), 1, (size_t)sz, f);
        fclose(f);
        return data;
    };

    // NOTE: these files will be added by CMake as generated outputs.
    std::vector<uint32_t> raygenSpv = loadSpv("raygen.rgen.spv");
    std::vector<uint32_t> missSpv = loadSpv("miss.rmiss.spv");

    vk::ShaderModule raygenModule = device.createShaderModule(
        vk::ShaderModuleCreateInfo{}.setCode(raygenSpv)
    );
    vk::ShaderModule missModule = device.createShaderModule(
        vk::ShaderModuleCreateInfo{}.setCode(missSpv)
    );

    // Stages
    std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = vk::PipelineShaderStageCreateInfo{}
        .setStage(vk::ShaderStageFlagBits::eRaygenKHR)
        .setModule(raygenModule)
        .setPName("main");
    stages[1] = vk::PipelineShaderStageCreateInfo{}
        .setStage(vk::ShaderStageFlagBits::eMissKHR)
        .setModule(missModule)
        .setPName("main");

    // Groups (raygen + miss)
    std::array<vk::RayTracingShaderGroupCreateInfoKHR, 2> groups{};
    groups[0] = vk::RayTracingShaderGroupCreateInfoKHR{}
        .setType(vk::RayTracingShaderGroupTypeKHR::eGeneral)
        .setGeneralShader(0)
        .setClosestHitShader(VK_SHADER_UNUSED_KHR)
        .setAnyHitShader(VK_SHADER_UNUSED_KHR)
        .setIntersectionShader(VK_SHADER_UNUSED_KHR);
    groups[1] = vk::RayTracingShaderGroupCreateInfoKHR{}
        .setType(vk::RayTracingShaderGroupTypeKHR::eGeneral)
        .setGeneralShader(1)
        .setClosestHitShader(VK_SHADER_UNUSED_KHR)
        .setAnyHitShader(VK_SHADER_UNUSED_KHR)
        .setIntersectionShader(VK_SHADER_UNUSED_KHR);

    vk::PhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    vk::PhysicalDeviceProperties2 props2{};
    props2.pNext = &rtProps;
    phys.getProperties2(&props2);

    vk::RayTracingPipelineCreateInfoKHR rtci{};
    rtci.setStages(stages);
    rtci.setGroups(groups);
    rtci.maxPipelineRayRecursionDepth = 1;
    rtci.layout = pipeLayout;

    vk::Pipeline rtPipeline = device.createRayTracingPipelineKHR({}, {}, rtci).value;

    // Shader binding table (SBT)
    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t handleAlign = rtProps.shaderGroupHandleAlignment;
    const uint32_t baseAlign = rtProps.shaderGroupBaseAlignment;

    auto alignUp = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };

    const uint32_t handleSizeAligned = alignUp(handleSize, handleAlign);
    const uint32_t raygenSbtSize = alignUp(handleSizeAligned, baseAlign);
    const uint32_t missSbtSize = alignUp(handleSizeAligned, baseAlign);

    Buffer sbt = createBuffer(
        phys, device,
        raygenSbtSize + missSbtSize,
        vk::BufferUsageFlagBits::eShaderBindingTableKHR | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        true
    );

    std::vector<uint8_t> handles(raygenSbtSize + missSbtSize);
    device.getRayTracingShaderGroupHandlesKHR(rtPipeline, 0, 2, handles.size(), handles.data());

    void* sbtMap = device.mapMemory(sbt.mem, 0, VK_WHOLE_SIZE);
    std::memcpy(sbtMap, handles.data(), handles.size());
    device.unmapMemory(sbt.mem);

    vk::BufferDeviceAddressInfo bdaInfo{};
    bdaInfo.buffer = sbt.buf;
    vk::DeviceAddress sbtAddress = device.getBufferAddress(bdaInfo);

    vk::StridedDeviceAddressRegionKHR raygenRegion{};
    raygenRegion.deviceAddress = sbtAddress;
    raygenRegion.stride = raygenSbtSize;
    raygenRegion.size = raygenSbtSize;

    vk::StridedDeviceAddressRegionKHR missRegion{};
    missRegion.deviceAddress = sbtAddress + raygenSbtSize;
    missRegion.stride = missSbtSize;
    missRegion.size = missSbtSize;

    vk::StridedDeviceAddressRegionKHR hitRegion{};
    vk::StridedDeviceAddressRegionKHR callableRegion{};

    // Indirect args buffer (VkTraceRaysIndirectCommandKHR)
    struct TraceArgs {
        uint32_t width, height, depth;
    };
    Buffer indirect = createBuffer(
        phys, device,
        sizeof(TraceArgs),
        vk::BufferUsageFlagBits::eIndirectBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        true
    );
    void* indMap = device.mapMemory(indirect.mem, 0, sizeof(TraceArgs));
    auto* args = reinterpret_cast<TraceArgs*>(indMap);
    args->width = 1;
    args->height = 1;
    args->depth = 1;
    device.unmapMemory(indirect.mem);

    vk::DeviceAddress indirectAddr = device.getBufferAddress(vk::BufferDeviceAddressInfo{}.setBuffer(indirect.buf));

    // Uniform buffer
    Buffer ubo = createBuffer(
        phys, device,
        256,
        vk::BufferUsageFlagBits::eUniformBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        false
    );

    struct FrameState {
        vk::CommandPool pool{};
        vk::DescriptorPool descPool{};
        vk::Fence fence{};
        vk::CommandBuffer cmd {};
    };

    auto createDescPool = [&]() {
        std::array<vk::DescriptorPoolSize, 3> sizes{};
        sizes[0] = {vk::DescriptorType::eUniformBuffer, 1024};
        sizes[1] = {vk::DescriptorType::eStorageImage, 1024};
        sizes[2] = {vk::DescriptorType::eAccelerationStructureKHR, 1024};
        return device.createDescriptorPool(vk::DescriptorPoolCreateInfo{}
            .setMaxSets(1024)
            .setPoolSizes(sizes));
    };

    std::array<FrameState, kFramesInFlight> frames{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        frames[i].pool = device.createCommandPool(vk::CommandPoolCreateInfo{}
            .setQueueFamilyIndex(q.graphics.value())
            .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer));
        frames[i].descPool = createDescPool();
        frames[i].fence = device.createFence(vk::FenceCreateInfo{vk::FenceCreateFlagBits::eSignaled});
    }

    uint64_t frameCounter = 0;
    uint64_t printed = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        uint32_t frameIndex = (uint32_t)(frameCounter % kFramesInFlight);
        auto& f = frames[frameIndex];

        (void)device.waitForFences(1, &f.fence, VK_TRUE, UINT64_MAX);
        device.resetFences(1, &f.fence);

        if (kResetCommandPoolPerFrame) {
            (void)device.resetCommandPool(f.pool, {});
        } else if (!kFreeCommandBufferPerFrame) {
            // Recreate command pool
            device.destroyCommandPool(f.pool);
            f.pool = device.createCommandPool(vk::CommandPoolCreateInfo{}
                .setQueueFamilyIndex(q.graphics.value())
                .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer));
        }
        if (kResetDescriptorPoolPerFrame) {
            // All sets become invalid.
            (void)device.resetDescriptorPool(f.descPool, {});
        }

        // Acquire image for present loop like main2.
        uint32_t imageIndex = 0;
        vk::Semaphore imageAvailable = imageAvailableSems[frameIndex];
        vk::Result acquireRes = device.acquireNextImageKHR(swapchain, UINT64_MAX, imageAvailable, {}, &imageIndex);
        if (acquireRes != vk::Result::eSuccess && acquireRes != vk::Result::eSuboptimalKHR) {
            continue;
        }
        vk::Semaphore renderFinished = renderFinishedSems[imageIndex];

        vk::CommandBuffer & cmd = f.cmd;

        if (kFreeCommandBufferPerFrame) {
            device.freeCommandBuffers(f.pool, cmd);
            cmd = vk::CommandBuffer{};
        }
        if (!cmd || !kFreeCommandBufferPerFrame) {
            cmd = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{}
                .setCommandPool(f.pool)
                .setLevel(vk::CommandBufferLevel::ePrimary)
                .setCommandBufferCount(1))[0];
        }

        cmd.begin(vk::CommandBufferBeginInfo{});

        // Transition storage image
        transitionImage(cmd, storageImg.img, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);

        // Write UBO frame number
        {
            void* p = device.mapMemory(ubo.mem, 0, 4);
            uint32_t x = (uint32_t)frameCounter;
            std::memcpy(p, &x, 4);
            device.unmapMemory(ubo.mem);
        }

        // Create+Update+Bind descriptor set each frame
        vk::DescriptorSet set0 = device.allocateDescriptorSets(
            vk::DescriptorSetAllocateInfo{}
                .setDescriptorPool(f.descPool)
                .setSetLayouts(set0Layout)
        )[0];

        vk::DescriptorBufferInfo uboInfo{};
        uboInfo.buffer = ubo.buf;
        uboInfo.offset = 0;
        uboInfo.range = 256;

        vk::DescriptorImageInfo imgInfo{};
        imgInfo.imageView = storageImg.view;
        imgInfo.imageLayout = vk::ImageLayout::eGeneral;

        vk::WriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &dummyTLAS;

        vk::WriteDescriptorSet writes[3]{};
        writes[0] = vk::WriteDescriptorSet{}
            .setDstSet(set0)
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eUniformBuffer)
            .setPBufferInfo(&uboInfo);
        writes[1] = vk::WriteDescriptorSet{}
            .setDstSet(set0)
            .setDstBinding(1)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageImage)
            .setPImageInfo(&imgInfo);
        writes[2] = vk::WriteDescriptorSet{}
            .setDstSet(set0)
            .setDstBinding(2)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eAccelerationStructureKHR)
            .setPNext(&asWrite);

        device.updateDescriptorSets(3, writes, 0, nullptr);

        cmd.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, rtPipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, pipeLayout, 0, set0, {});

        cmd.traceRaysIndirectKHR(&raygenRegion, &missRegion, &hitRegion, &callableRegion, indirectAddr);

        // Minimal present work to keep window alive
        {
            // Transition swapchain to present (no rendering)
            if (swapchainLayouts[imageIndex] != vk::ImageLayout::ePresentSrcKHR) {
                vk::ImageMemoryBarrier toPresent{};
                toPresent.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                toPresent.dstAccessMask = {};
                toPresent.oldLayout = swapchainLayouts[imageIndex];
                toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
                toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toPresent.image = swapchainImages[imageIndex];
                toPresent.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, toPresent);
                swapchainLayouts[imageIndex] = vk::ImageLayout::ePresentSrcKHR;
            }
        }

        cmd.end();

        vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
        vk::SubmitInfo si{};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderFinished;
        graphicsQ.submit(1, &si, f.fence);

        vk::PresentInfoKHR pi{};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        (void)presentQ.presentKHR(pi);

        ++frameCounter;
        if (frameCounter / 1000 != printed) {
            printed = frameCounter / 1000;
            std::cout << "Frame " << frameCounter << "\n";
        }
    }

    device.waitIdle();

    // Cleanup
    for (auto& f : frames) {
        device.destroyFence(f.fence);
        device.destroyDescriptorPool(f.descPool);
        device.destroyCommandPool(f.pool);
    }

    device.destroyBuffer(ubo.buf);
    device.freeMemory(ubo.mem);

    device.destroyBuffer(indirect.buf);
    device.freeMemory(indirect.mem);

    device.destroyBuffer(sbt.buf);
    device.freeMemory(sbt.mem);

    device.destroyImageView(storageImg.view);
    device.destroyImage(storageImg.img);
    device.freeMemory(storageImg.mem);

    device.destroyPipeline(rtPipeline);
    device.destroyPipelineLayout(pipeLayout);
    device.destroyDescriptorSetLayout(set0Layout);

    device.destroyShaderModule(raygenModule);
    device.destroyShaderModule(missModule);

    device.destroySwapchainKHR(swapchain);
    instance.destroySurfaceKHR(surface);

    device.destroy();
    instance.destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
