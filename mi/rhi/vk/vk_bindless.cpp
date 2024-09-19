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
#include "vk_buffer.h"

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
                    (uint32_t)RHIBindlessResourceType::kMaxAndImmSampler, {}, stages,
                    immutable_samplers
            },
            {
                    (uint32_t)RHIBindlessResourceType::kUniformBuffer, vk::DescriptorType::eUniformBuffer,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kUniformBuffer].size), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kStorageBuffer, vk::DescriptorType::eStorageBuffer,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kStorageBuffer].size), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kSRV, vk::DescriptorType::eSampledImage,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kSRV].size), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kUAV, vk::DescriptorType::eStorageImage,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kUAV].size), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kAccelerationStructure, vk::DescriptorType::eAccelerationStructureKHR,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kAccelerationStructure].size), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kSampler, vk::DescriptorType::eSampler,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kSampler].size), stages
            }
    };
    auto layout_create_info = vk::DescriptorSetLayoutCreateInfo {{}, layout_bindings};
    bindless_descriptor_set_layout_ = device.createDescriptorSetLayout(layout_create_info);
    mi_assert(bindless_descriptor_set_layout_, "Failed to create descriptor layout");

    // Pool
    {
        vk::DescriptorPoolSize pool_sizes[(int)RHIBindlessResourceType::kMaxAndImmSampler];
        for(int i = 0; i < (int)RHIBindlessResourceType::kMaxAndImmSampler; i++) {
            auto & pool_size = pool_sizes[i];
            pool_size = {
                    GetVulkanDescriptorType((RHIBindlessResourceType)i),
                    (uint32_t)bindless_channels_[i].size
            };
        }
        // Add immutable samplers
        pool_sizes[(int)RHIBindlessResourceType::kSampler].descriptorCount
            += C::kNumDefaultBindlessImmutableSamplers;
        // Multipy by the number of sets
        for(auto & pool_size : pool_sizes) {
            pool_size.descriptorCount *= (uint32_t)std::size(bindless_descriptor_sets_);
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

void VulkanBindlessManager::Initialize_RHIThread() {
    // Do nothing.
}

void VulkanBindlessManager::Destroy_RHIThread() {
    // Do nothing.
}

void VulkanBindlessManager::FreeResourceSlotRHI(RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) {
    // Write null descriptor sets
    auto device = GetVulkanRHI()->GetDevice();
    auto null_descriptor = vk::WriteDescriptorSet {
        bindless_descriptor_sets_[set_index_],
        static_cast<uint32_t>(type),
        slot,
        num_slots,
        GetVulkanDescriptorType(type),
        nullptr,
        nullptr
    };
    device.updateDescriptorSets({null_descriptor}, {});
}

void VulkanBindlessManager::CommitResourceSlotUpdateRHI(RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) {
    auto device = GetVulkanRHI()->GetDevice();
    auto descriptor = vk::WriteDescriptorSet {
        bindless_descriptor_sets_[set_index_],
        static_cast<uint32_t>(type),
        slot,
        num_slots,
        GetVulkanDescriptorType(type)
    };
    if(type == RHIBindlessResourceType::kStorageBuffer || type == RHIBindlessResourceType::kUniformBuffer) {
        int ext_bc = type == RHIBindlessResourceType::kStorageBuffer ? 0 : 1;
        auto updates = reinterpret_cast<vk::DescriptorBufferInfo *>(update_descriptor_set_buffer);
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto buffer = (VulkanBuffer*)(ref.Raw());
            updates[i] = vk::DescriptorBufferInfo {
                buffer->GetBuffer(),
                bindless_buffer_channel[ext_bc].offsets[slot + i],
                ((size_t)-1ll == bindless_buffer_channel[ext_bc].sizes[slot + i])
                ? VK_WHOLE_SIZE : bindless_buffer_channel[ext_bc].sizes[slot + i]
            };
        }
        descriptor.setPBufferInfo(updates);
    } else if(type == RHIBindlessResourceType::kUAV || type == RHIBindlessResourceType::kSRV) {
        auto updates = reinterpret_cast<vk::DescriptorImageInfo *>(update_descriptor_set_buffer);
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto texture = (VulkanTexture*)(ref.Raw());
            vk::ImageLayout ready_layout =
                    type == RHIBindlessResourceType::kSRV
                    ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eGeneral;
            if(!texture->IsBindlessUseOptimalAccess()) ready_layout = vk::ImageLayout::eGeneral;
            updates[i] = vk::DescriptorImageInfo {
                nullptr,
                texture->GetImageView(),
                ready_layout
            };
        }
        descriptor.setPImageInfo(updates);
    } else if(type == RHIBindlessResourceType::kAccelerationStructure) {
        auto updates = reinterpret_cast<vk::WriteDescriptorSetAccelerationStructureKHR *>(update_descriptor_set_buffer);
        auto as_ptrs = reinterpret_cast<vk::AccelerationStructureKHR *>(
                update_descriptor_set_buffer
                + sizeof(vk::WriteDescriptorSetAccelerationStructureKHR));
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto as = (VulkanAccelerationStructure*)(ref.Raw());
            as_ptrs[i] = as->GetAccelerationStructure();
        }
        updates->setAccelerationStructureCount(num_slots);
        updates->setPAccelerationStructures(as_ptrs);
        descriptor.setPNext(updates);
    } else if(type == RHIBindlessResourceType::kSampler) {
        auto updates = reinterpret_cast<vk::DescriptorImageInfo *>(update_descriptor_set_buffer);
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto sampler = (VulkanSampler*)(ref.Raw());
            updates[i] = vk::DescriptorImageInfo {
                sampler->GetSampler()
            };
        }
        descriptor.setPImageInfo(updates);
    }
    device.updateDescriptorSets({descriptor}, {});
}

void VulkanBindlessManager::SwapSets_RHIThread () {
    set_index_ ++;
    auto device = GetVulkanRHI()->GetDevice();
    // Copy the previous set to the new set
    uint32_t num_descriptors = 0;
    // Copy all the descriptors
    for(auto & channel : bindless_channels_) {
        num_descriptors += channel.size;
    }
    auto copy = vk::CopyDescriptorSet {
        bindless_descriptor_sets_[set_index_ ^ 1],
        0,
        0,
        bindless_descriptor_sets_[set_index_],
        0,
        0,
        num_descriptors
    };
    device.updateDescriptorSets({}, copy);
}

MI_NAMESPACE_END