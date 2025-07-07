/*
 * Created: 2025/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
#include "vk_rhi.h"
#include "vk_cmd_exec.h"
#include "vk_resource.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_conversion.h"
#include "vk_bindless.h"

// On NVIDIA hardware, vkResetCommandPool become very slow after multiple frames
// I tried to add a command buffer reset flag to the command pool upon creation
// (https://github.com/vulkano-rs/vulkano/issues/1521) but that further slowed
// down the entire program. So I just recreated the command pool every frame.
// https://github.com/GPUOpen-Drivers/xgl/issues/63

// False for recreating the command pool every frame.
// True for using vkResetCommandPool
#define RESET_COMMAND_POOL false


MI_NAMESPACE_BEGIN

void VulkanCommandExecutor::CommandQueueState::Init(RHICommandQueueType type) {
    CHECK_RHI_THREAD();
    {
        assert(cmd_pool == nullptr);
        assert(cmd == nullptr);

        auto rhi = GetVulkanRHI();
        cmd_pool = rhi->GetDevice().createCommandPool(
                vk::CommandPoolCreateInfo{
                        vk::CommandPoolCreateFlagBits::eTransient,
                        rhi->GetQueueFamilyIndex(type)
                }
        );
        // Clear bound vertex buffers
        for(auto & span = bound_vertex_buffers; auto & buf : span)
            buf = RHIBufferSpan{};
        // Initialize all bind point states
        for(auto [i, point] : std::views::enumerate(points)) {
            point.bound_private_descriptor_set = nullptr;
            point.bound_pipeline = nullptr;
            point.bound_descriptor_dirty = true;
            point.bound_pipeline_dirty = true;
            point.parameter_table = {};
            point.bind_point_type = (RHIBindPointType) i;
        }
        // We reset the descriptor pool every frame.
        vk::DescriptorPoolSize pool_sizes[] = {
                {
                        vk::DescriptorType::eUniformBuffer,
                        C::kMaxNumUniformBufferDescriptorsPerFrame
                },
                {
                        vk::DescriptorType::eStorageBuffer,
                        C::kMaxNumStorageBufferDescriptorsPerFrame
                },
                {
                        vk::DescriptorType::eSampledImage,
                        C::kMaxNumSampledTextureDescriptorsPerFrame
                },
                {
                        vk::DescriptorType::eStorageImage,
                        C::kMaxNumStorageTextureDescriptorsPerFrame
                },
                {
                        vk::DescriptorType::eAccelerationStructureKHR,
                        C::kMaxNumAccelerationStructureDescriptorsPerFrame
                },
                {
                        vk::DescriptorType::eSampler,
                        C::kMaxNumSamplerDescriptorsPerFrame
                }
        };
        descriptor_pool = rhi->GetDevice().createDescriptorPool(
                vk::DescriptorPoolCreateInfo{
                        {},
                        C::kMaxNumDescriptorSetsPerFrame,
                        pool_sizes
                }
        );
    }
}

void VulkanCommandExecutor::CommandQueueState::BindPoint::Destroy() {
    // ...
}


void VulkanCommandExecutor::CommandQueueState::Destroy() {
    CHECK_RHI_THREAD();
    auto rhi = GetVulkanRHI();
    for(auto & point : points) {
        point.Destroy();
    }
    CloseCmd();
    rhi->GetDevice().destroyDescriptorPool(descriptor_pool);
    rhi->GetDevice().destroyCommandPool(cmd_pool);
}

void VulkanCommandExecutor::CommandQueueState::Clear(bool return_resources_to_system) {
    CHECK_RHI_THREAD();
    // Clear states, get ready for the next frame.
    auto rhi = GetVulkanRHI();
    {
        memset(bound_vertex_buffers, 0, sizeof(bound_vertex_buffers));
        bound_index_buffer = {};
        bound_index_type = RHIIndexType::kMax;
        draw_state_.Reset();
    }
    for(auto & point : points) {
        point.bound_private_descriptor_set = nullptr;
        point.bound_pipeline = nullptr;
        point.bound_descriptor_dirty = true;
        point.bound_pipeline_dirty = true;
        point.parameter_table = {};
    }
    // Reset the temporary allocator
    allocator.Reset();
    // This should always be true as a frame should end with FrameEnd(), which contains a submit.
    assert(cmd == nullptr);
    // Sometimes we do not return resources back to the system. We may be able to reuse them in the later frames.
    {
        if constexpr (RESET_COMMAND_POOL) {
            rhi->GetDevice().resetCommandPool(cmd_pool,
                          return_resources_to_system
                          ? vk::CommandPoolResetFlagBits::eReleaseResources : vk::CommandPoolResetFlagBits{});
        } else { rhi->GetDevice().destroy(cmd_pool);
            cmd_pool = rhi->GetDevice().createCommandPool(
                    vk::CommandPoolCreateInfo{
                            vk::CommandPoolCreateFlagBits::eTransient,
                            rhi->GetQueueFamilyIndex(RHICommandQueueType::kGraphics)
                    }
            );
        }
    }
    {
        rhi->GetDevice().resetDescriptorPool(descriptor_pool);
    }
}

void VulkanCommandExecutor::CommandQueueState::BindIndexBuffer(RHIBufferSpan span, RHIIndexType type) {
    if (bound_index_buffer != span || bound_index_type != type) {
        auto vk_buffer = static_cast<VulkanBuffer*>(span.buffer)->GetBuffer();
        cmd.bindIndexBuffer(vk_buffer, span.offset, GetVulkanIndexType(type));
        bound_index_buffer = span;
        bound_index_type = type;
    }
}


void VulkanCommandExecutor::CommandQueueState::BeginCmd () {
    if(!cmd_recording_started) {
        assert(cmd == nullptr);
        cmd_recording_started = true;
        auto device = GetVulkanRHI()->GetDevice();
        // Allocate a new command buffer
        // TODO accelerate this?
        cmd = device.allocateCommandBuffers(
                vk::CommandBufferAllocateInfo{
                        cmd_pool,
                        vk::CommandBufferLevel::ePrimary,
                        1
                }
        )[0];
        cmd.begin(vk::CommandBufferBeginInfo{});
        // Setup default dynamic states.
        SetupDefaultDynamicStates();
    }
}

bool VulkanCommandExecutor::CommandQueueState::CloseCmd () {
    if(cmd_recording_started) {
        cmd_recording_started = false;
        cmd.end();
        return true;
    }
    return false;
}

void VulkanCommandExecutor::CommandQueueState::SetupDefaultDynamicStates() const {
    // TODO provide a way to dynamically set these values
    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setDepthBiasEnable(false);
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
}

MI_NAMESPACE_END