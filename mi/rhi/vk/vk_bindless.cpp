/*
 * Created: 2024/7/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/constants.h"
#include "core/infra.h"
#include "vk_bindless.h"
#include "vk_conversion.h"
#include "vk_resource.h"
#include "vk_texture.h"

MI_NAMESPACE_BEGIN

VulkanBindlessManager::VulkanBindlessManager() : RHIBindlessManager() {
    auto stages =
            vk::ShaderStageFlagBits::eAllGraphics | vk::ShaderStageFlagBits::eCompute
        | vk::ShaderStageFlagBits::eRaygenKHR | vk::ShaderStageFlagBits::eAnyHitKHR
        | vk::ShaderStageFlagBits::eClosestHitKHR | vk::ShaderStageFlagBits::eMissKHR;
    auto device = GetVulkanRHI()->GetDevice();
    {
        auto default_sampler = vk::SamplerCreateInfo {
                {},
                vk::Filter::eLinear,
                vk::Filter::eLinear,
                vk::SamplerMipmapMode::eLinear,
                vk::SamplerAddressMode::eRepeat,
                vk::SamplerAddressMode::eRepeat,
                vk::SamplerAddressMode::eRepeat,
                0.0f,
                VK_TRUE,
                16.0f,
                VK_FALSE,
                vk::CompareOp::eNever,
                0.0f,
                0.0f,
                vk::BorderColor::eFloatOpaqueWhite,
                VK_FALSE
        };
        immutable_samplers_.linear_wrap = device.createSampler(default_sampler);
        default_sampler.minFilter = vk::Filter::eNearest;
        default_sampler.magFilter = vk::Filter::eNearest;
        default_sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
        immutable_samplers_.nearest_wrap = device.createSampler(default_sampler);
        default_sampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        default_sampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        default_sampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        immutable_samplers_.nearest_clamp_edge = device.createSampler(default_sampler);
        default_sampler.minFilter = vk::Filter::eLinear;
        default_sampler.magFilter = vk::Filter::eLinear;
        default_sampler.mipmapMode = vk::SamplerMipmapMode::eLinear;
        immutable_samplers_.linear_clamp_edge = device.createSampler(default_sampler);
    }
    std::array immutable_samplers = {
            immutable_samplers_.linear_wrap,
            immutable_samplers_.linear_clamp_edge,
            immutable_samplers_.nearest_wrap,
            immutable_samplers_.nearest_clamp_edge
    };
    static_assert(std::size(immutable_samplers) == C::kNumDefaultBindlessImmutableSamplers);
    vk::DescriptorSetLayoutBinding layout_bindings[] = {
            {
                    0, {}, stages,
                    immutable_samplers
            },
            {
                    0, vk::DescriptorType::eUniformBuffer,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kUniformBuffer].size), stages
            },
            {
                    (int)RHIBindlessResourceType::kStorageBuffer, vk::DescriptorType::eStorageBuffer,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kStorageBuffer].size), stages
            },
            {
                0, vk::DescriptorType::eSampledImage,
                static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kSRV].size), stages
            },
            {
                0, vk::DescriptorType::eStorageImage,
                static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kUAV].size), stages
            },
            {
                0, vk::DescriptorType::eAccelerationStructureKHR,
                static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kAccelerationStructure].size), stages
            },
            {
                0, vk::DescriptorType::eSampler,
                static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kSampler].size), stages
            }
    };
    // Record binding offsets of different channels within the layout
    for(int i = 1; i <= std::size(layout_bindings); i++) {
        uint32_t offset = layout_bindings[i - 1].descriptorCount + layout_bindings[i-1].binding;
        channel_binding_offsets_[(int)FromVulkanDescriptorType(layout_bindings[i].descriptorType)]
         = offset;
        if(i != std::size(layout_bindings)) {
            layout_bindings[i].binding = offset;
        }
    }
    auto layout_create_info = vk::DescriptorSetLayoutCreateInfo {
            {},
            layout_bindings
    };
    bindless_descriptor_set_layout_ = device.createDescriptorSetLayout(layout_create_info);
    mi_assert(bindless_descriptor_set_layout_, "Failed to create descriptor layout");

    // Pool
    {
        vk::DescriptorPoolSize pool_sizes[] = {
            {vk::DescriptorType::eSampledImage,
             (uint32_t)bindless_channels_[static_cast<int>(RHIBindlessResourceType::kSRV)].size},
            {vk::DescriptorType::eStorageImage,
             (uint32_t)bindless_channels_[static_cast<int>(RHIBindlessResourceType::kUAV)].size},
            {vk::DescriptorType::eUniformBuffer,
            (uint32_t)bindless_channels_[static_cast<int>(RHIBindlessResourceType::kUniformBuffer)].size},
            {vk::DescriptorType::eStorageBuffer,
             (uint32_t)bindless_channels_[static_cast<int>(RHIBindlessResourceType::kStorageBuffer)].size},
            {vk::DescriptorType::eAccelerationStructureKHR,
             (uint32_t)bindless_channels_[static_cast<int>(RHIBindlessResourceType::kAccelerationStructure)].size},
            // Still we need to reserve for immutable samplers
             {vk::DescriptorType::eSampler,
             (uint32_t)bindless_channels_[static_cast<int>(RHIBindlessResourceType::kSampler)].size + C::kNumDefaultBindlessImmutableSamplers},
        };
        // Multipy by the number of sets
        for(auto & pool_size : pool_sizes) {
            pool_size.descriptorCount *= std::size(bindless_descriptor_sets_);
        }
        bindless_descriptor_pool_ = device.createDescriptorPool(
                vk::DescriptorPoolCreateInfo{
                        vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
                        static_cast<uint32_t>(std::size(bindless_descriptor_sets_)),
                        pool_sizes
                }
        );
        mi_assert(bindless_descriptor_pool_, "Failed to create descriptor pool");
    }
    // Allocate descriptor sets
    {
        auto sets = device.allocateDescriptorSets(
                vk::DescriptorSetAllocateInfo{
                        bindless_descriptor_pool_,
                        static_cast<uint32_t>(std::size(bindless_descriptor_sets_)),
                        &bindless_descriptor_set_layout_
                }
        );
        mi_assert(sets.size() == std::size(bindless_descriptor_sets_), "Failed to allocate descriptor sets");
        std::copy(sets.begin(), sets.end(), bindless_descriptor_sets_);
    }
}

vk::DescriptorSetLayout VulkanBindlessManager::GetBindlessDescriptorSetLayout() {
    return bindless_descriptor_set_layout_;
}

vk::DescriptorSet VulkanBindlessManager::GetBindlessDescriptorSet() {
    return bindless_descriptor_sets_[set_index_];
}

VulkanBindlessManager::~VulkanBindlessManager() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroyDescriptorPool(bindless_descriptor_pool_);
    device.destroyDescriptorSetLayout(bindless_descriptor_set_layout_);
    device.destroy(immutable_samplers_.linear_wrap);
    device.destroy(immutable_samplers_.linear_clamp_edge);
    device.destroy(immutable_samplers_.nearest_wrap);
    device.destroy(immutable_samplers_.nearest_clamp_edge);
}

void VulkanBindlessManager::UseResource(
        vk::CommandBuffer cmd, RHIBindlessResourceType type, int slot_index,
        vk::PipelineStageFlags use_stages
) {
    // Only try to transit image layouts
    if(type == RHIBindlessResourceType::kSRV) {
        auto & channel = bindless_channels_[static_cast<int>(RHIBindlessResourceType::kSRV)];
        auto img = channel.resource_refs[slot_index - L];
        if(img) {
            ((VulkanTexture*)img.Raw())->Use(cmd, vk::ImageLayout::eShaderReadOnlyOptimal, use_stages);
        }
    }
}

void VulkanBindlessManager::Initialize_RHIThread() {
    // Do nothing.
}

void VulkanBindlessManager::Destroy_RHIThread() {
    // Do nothing.
}

void VulkanBindlessManager::UpdateResourceSlotRHI(RHIBindlessResourceType type, uint32_t slot) {

    auto write = vk::WriteDescriptorSet {
        bindless_descriptor_sets_[set_index_],
        ,
        slot,
        1,
        vk::DescriptorType::eUniformBuffer,
        nullptr,
        &bindless_channels_[static_cast<int>(type)][slot].buffer_info
    };

}

void VulkanBindlessManager::SwapSets_RHIThread () {
    set_index_ ++;
    auto device = GetVulkanRHI()->GetDevice();
    // Copy the previous set to the new set
    device.updateDescriptorSets(
        {
            vk::CopyDescriptorSet(
                bindless_descriptor_sets_[set_index_],
                0,
                0,
                bindless_descriptor_sets_[set_index_ ^ 1],
                0,
                0,
                1
            )
        },
        {}
    );
}

MI_NAMESPACE_END