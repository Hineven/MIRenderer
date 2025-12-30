// Minimal Vulkan repro: swapchain + double-buffered command pools
// Toggle kResetCommandPoolPerFrame to compare:
//   true  -> reset command pool each frame slot (no vkFreeCommandBuffers)
//   false -> vkFreeCommandBuffers each frame (no vkResetCommandPool)
//
// Goal: isolate potential NVIDIA driver memory growth differences.

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

static constexpr uint32_t kWidth = 1280;
static constexpr uint32_t kHeight = 720;
static constexpr uint32_t kFramesInFlight = 2;

// true: reset pool and allocate (no free)
// false: free command buffer each frame (no pool reset)
static constexpr bool kResetCommandPoolPerFrame = true;

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;

    [[nodiscard]] bool complete() const { return graphics.has_value() && present.has_value(); }
};

static QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice phys, vk::SurfaceKHR surface) {
    QueueFamilyIndices out{};
    auto props = phys.getQueueFamilyProperties();

    for (uint32_t i = 0; i < static_cast<uint32_t>(props.size()); ++i) {
        if ((props[i].queueFlags & vk::QueueFlagBits::eGraphics) && !out.graphics.has_value()) {
            out.graphics = i;
        }
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
    // Prefer SRGB if available, otherwise pick first.
    for (const auto& f : formats) {
        if (f.format == vk::Format::eB8G8R8A8Srgb && f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) return f;
    }
    return formats.empty() ? vk::SurfaceFormatKHR{} : formats[0];
}

static vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& modes) {
    // FIFO is always supported. MAILBOX if available.
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

int main() {
    VULKAN_HPP_DEFAULT_DISPATCHER.init();

    if (!glfwInit()) {
        std::cerr << "glfwInit failed\n";
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow* window = glfwCreateWindow(kWidth, kHeight, "min_capture_test2", nullptr, nullptr);
    if (!window) {
        std::cerr << "glfwCreateWindow failed\n";
        glfwTerminate();
        return 1;
    }

    // ---- Instance ----
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> instExts;
    instExts.reserve(glfwExtCount + 1);
    for (uint32_t i = 0; i < glfwExtCount; ++i) {
        instExts.push_back(glfwExts[i]);
    }
#ifndef NDEBUG
    instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    vk::ApplicationInfo appInfo{};
    appInfo.pApplicationName = "min_capture_test2";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> layers;

    vk::InstanceCreateInfo ici{};
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = static_cast<uint32_t>(instExts.size());
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();


    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
    vk::Instance instance = vk::createInstance(ici);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

    // ---- Surface ----
    VkSurfaceKHR rawSurface{};
    if (glfwCreateWindowSurface(instance, window, nullptr, &rawSurface) != VK_SUCCESS) {
        std::cerr << "glfwCreateWindowSurface failed\n";
        return 1;
    }
    vk::SurfaceKHR surface(rawSurface);

    // ---- Physical device ----
    auto physDevices = instance.enumeratePhysicalDevices();
    if (physDevices.empty()) {
        std::cerr << "No Vulkan physical devices found\n";
        return 1;
    }

    vk::PhysicalDevice phys = physDevices[0];
    for (auto d : physDevices) {
        if (d.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
            phys = d;
            break;
        }
    }

    QueueFamilyIndices q = findQueueFamilies(phys, surface);
    if (!q.complete()) {
        std::cerr << "No suitable queue families (graphics+present)\n";
        return 1;
    }

    // ---- Device ----
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

    std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    vk::PhysicalDeviceFeatures feats{};

    vk::DeviceCreateInfo dci{};
    dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    dci.pEnabledFeatures = &feats;

#ifndef NDEBUG
    dci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    dci.ppEnabledLayerNames = layers.data();
#endif

    vk::Device device = phys.createDevice(dci);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    vk::Queue graphicsQ = device.getQueue(q.graphics.value(), 0);
    vk::Queue presentQ = device.getQueue(q.present.value(), 0);

    // ---- Swapchain ----
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
        sci.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
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
    if (swapchainImages.empty()) {
        std::cerr << "Swapchain has no images\n";
        return 1;
    }

    // Track swapchain image layout for correctness (present requires PRESENT_SRC_KHR).
    std::vector<vk::ImageLayout> swapchainLayouts(swapchainImages.size(), vk::ImageLayout::eUndefined);

    // ---- Sync: per-frame fence + per-swapchain-image semaphores ----
    vk::FenceCreateInfo fenceCI{vk::FenceCreateFlagBits::eSignaled};

    vk::SemaphoreCreateInfo semCI{};

    // imageAvailable: per-frame is safe because we only reuse it after waiting the per-frame fence.
    // renderFinished: must not be reused until *that swapchain image* is re-acquired, so we keep it per-image.
    std::array<vk::Semaphore, kFramesInFlight> imageAvailableSems{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        imageAvailableSems[i] = device.createSemaphore(semCI);
    }
    std::vector<vk::Semaphore> renderFinishedSems(swapchainImages.size());
    for (size_t i = 0; i < swapchainImages.size(); ++i) {
        renderFinishedSems[i] = device.createSemaphore(semCI);
    }

    struct Frame {
        vk::CommandPool pool{};
        vk::Fence fence{};
    };

    std::array<Frame, kFramesInFlight> frames{};
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        vk::CommandPoolCreateInfo cpci{};
        cpci.queueFamilyIndex = q.graphics.value();
        // We want to allocate per-frame, and optionally reset pool.
        cpci.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        frames[i].pool = device.createCommandPool(cpci);

        frames[i].fence = device.createFence(fenceCI);
    }

    uint64_t frameCounter = 0;

    auto frame_count = 0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        frame_count ++;
        if (frame_count % 1000 == 0) {
            printf("Frame: %d\n", frame_count);
        }

        uint32_t frameIndex = static_cast<uint32_t>(frameCounter % kFramesInFlight);
        Frame& f = frames[frameIndex];

        // Ensure previous GPU work using this frame slot is complete.
        (void)device.waitForFences(1, &f.fence, VK_TRUE, UINT64_MAX);
        device.resetFences(1, &f.fence);

        // Reclaim cmd buffer memory according to mode.
        if (kResetCommandPoolPerFrame) {
            // Must only reset after fence wait.
            (void)device.resetCommandPool(f.pool, {});
        }

        // Acquire an image.
        uint32_t imageIndex = 0;
        vk::Semaphore imageAvailable = imageAvailableSems[frameIndex];

        vk::Result acquireRes = device.acquireNextImageKHR(swapchain, UINT64_MAX, imageAvailable, {}, &imageIndex);
        if (acquireRes == vk::Result::eErrorOutOfDateKHR) {
            continue;
        }
        if (acquireRes != vk::Result::eSuccess && acquireRes != vk::Result::eSuboptimalKHR) {
            continue;
        }

        vk::Semaphore renderFinished = renderFinishedSems[imageIndex];

        // Allocate command buffer each frame.
        vk::CommandBufferAllocateInfo cbai{};
        cbai.commandPool = f.pool;
        cbai.level = vk::CommandBufferLevel::ePrimary;
        cbai.commandBufferCount = 1;
        vk::CommandBuffer cmd = device.allocateCommandBuffers(cbai)[0];

        // Record.
        cmd.begin(vk::CommandBufferBeginInfo{});

        // Transition to TRANSFER_DST for a tiny clear via vkCmdClearColorImage.
        // (Avoid render pass/pipeline setup; keep it as clean as possible.)
        {
            vk::ImageMemoryBarrier toTransfer{};
            toTransfer.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
                ;
            toTransfer.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
            toTransfer.oldLayout = swapchainLayouts[imageIndex];
            toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = swapchainImages[imageIndex];
            toTransfer.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

            cmd.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eTransfer,
                {}, {}, {}, toTransfer);
            swapchainLayouts[imageIndex] = vk::ImageLayout::eTransferDstOptimal;
        }

        {
            vk::ClearColorValue color(std::array<float, 4>{0.05f, 0.05f, 0.08f, 1.0f});
            vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            cmd.clearColorImage(swapchainImages[imageIndex], vk::ImageLayout::eTransferDstOptimal, color, range);
        }

        // Transition to PRESENT.
        {
            vk::ImageMemoryBarrier toPresent{};
            toPresent.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
            toPresent.dstAccessMask = {};
            toPresent.oldLayout = vk::ImageLayout::eTransferDstOptimal;
            toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
            toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toPresent.image = swapchainImages[imageIndex];
            toPresent.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

            cmd.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eBottomOfPipe,
                {}, {}, {}, toPresent);
            swapchainLayouts[imageIndex] = vk::ImageLayout::ePresentSrcKHR;
        }

        cmd.end();

        // Submit.
        vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eTransfer;
        vk::SubmitInfo si{};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderFinished;

        graphicsQ.submit(1, &si, f.fence);

        // Present.
        vk::PresentInfoKHR pi{};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderFinished;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        (void)presentQ.presentKHR(pi);

        // Free command buffer only in the "free" mode.
        if (!kResetCommandPoolPerFrame) {
            device.freeCommandBuffers(f.pool, 1, &cmd);
        }

        ++frameCounter;
    }

    device.waitIdle();

    // Cleanup.
    for (auto& f : frames) {
        device.destroyFence(f.fence);
        device.destroyCommandPool(f.pool);
    }
    for (auto s : imageAvailableSems) device.destroySemaphore(s);
    for (auto s : renderFinishedSems) device.destroySemaphore(s);

    device.destroySwapchainKHR(swapchain);
    instance.destroySurfaceKHR(surface);

    device.destroy();
    instance.destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

