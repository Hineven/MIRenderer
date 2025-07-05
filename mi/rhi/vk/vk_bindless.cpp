/*
 * Created: 2024/7/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "core/constants.h"
#include "core/infra.h"
#include "vk_bindless.h"
#include "vk_conversion.h"
#include "vk_resource.h"
#include "vk_as.h"
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
                    (uint32_t)RHIBindlessResourceType::kReadOnlyStorageBuffer, vk::DescriptorType::eStorageBuffer,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kReadOnlyStorageBuffer].total_count), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kSRV, vk::DescriptorType::eSampledImage,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kSRV].total_count), stages
            },
            {
                    (uint32_t)RHIBindlessResourceType::kAccelerationStructure, vk::DescriptorType::eAccelerationStructureKHR,
                    static_cast<uint32_t>(bindless_channels_[(int)RHIBindlessResourceType::kAccelerationStructure].total_count), stages
            }
    };
    auto layout_create_info = vk::DescriptorSetLayoutCreateInfo {
        vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
        layout_bindings
    };
    bindless_descriptor_set_layout_ = device.createDescriptorSetLayout(layout_create_info);
    mi_assert(bindless_descriptor_set_layout_, "Failed to create descriptor layout");

    // Pool
    {
        vk::DescriptorPoolSize pool_sizes[(int)RHIBindlessResourceType::kMax];
        for(int i = 0; i < (int)RHIBindlessResourceType::kMax; i++) {
            auto & pool_size = pool_sizes[i];
            pool_size = {
                    GetVulkanDescriptorType((RHIBindlessResourceType)i),
                    (uint32_t)bindless_channels_[i].total_count
            };
        }
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
        vk::DescriptorSetLayout layouts[] = {bindless_descriptor_set_layout_, bindless_descriptor_set_layout_};
        auto sets = device.allocateDescriptorSets(
                vk::DescriptorSetAllocateInfo{
                        bindless_descriptor_pool_,
                        layouts
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
        {},
        static_cast<uint32_t>(type),
        slot,
        num_slots,
        GetVulkanDescriptorType(type),
        nullptr,
        nullptr
    };
    auto ptr_sets = bindless_descriptor_sets_;
    auto ptr_set_index = &set_index_;

    EnqueueRHIThreadTask([null_descriptor, ptr_sets, ptr_set_index]() {
        auto device = GetVulkanRHI()->GetDevice();
        auto curr_write = null_descriptor;
        curr_write.dstSet = ptr_sets[*ptr_set_index];
        device.updateDescriptorSets({curr_write}, {});
    });
}

void VulkanBindlessManager::CommitResourceSlotUpdateRHI(RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) {
    assert(GetCurrentThreadType() == ThreadType::kRenderThread);
    auto & queue = GetVulkanRHI()->GetGraphicsCommandQueue();
    auto descriptor_write = vk::WriteDescriptorSet {
        // Memory read is deferred to RHI thread
        {},//bindless_descriptor_sets_[set_index_],
        static_cast<uint32_t>(type),
        slot,
        num_slots,
        GetVulkanDescriptorType(type)
    };
    if(type == RHIBindlessResourceType::kReadOnlyStorageBuffer) {
        auto updates = queue.Allocate<vk::DescriptorBufferInfo[]>(num_slots);
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto buffer = (VulkanBuffer*)(ref.Raw());
            updates[i] = vk::DescriptorBufferInfo {
                buffer ? buffer->GetBuffer() : nullptr,
                0,
                VK_WHOLE_SIZE
            };
        }
        descriptor_write.setPBufferInfo(updates);
    } else if(type == RHIBindlessResourceType::kSRV) {
        auto updates = queue.Allocate<vk::DescriptorImageInfo[]>(num_slots);
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto texture = (VulkanTexture*)(ref.Raw());
            // mi_assert(texture->GetLayout() == RHITextureLayoutType::kShaderReadOnlyOptimal,
            //           "Bindless SRV texture layout must be shader read only optimal");
            vk::ImageLayout ready_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            updates[i] = vk::DescriptorImageInfo {
                nullptr,
                texture ? texture->GetImageView() : nullptr,
                ready_layout
            };
        }
        descriptor_write.setPImageInfo(updates);
    } else if(type == RHIBindlessResourceType::kAccelerationStructure) {
        auto updates = queue.Allocate<vk::WriteDescriptorSetAccelerationStructureKHR>(1);
        auto as_ptrs = queue.Allocate<vk::AccelerationStructureKHR[]>(num_slots);
        for(uint32_t i = 0; i < num_slots; i++) {
            auto ref = bindless_channels_[static_cast<int>(type)].resource_refs[slot + i];
            auto as = (VulkanAccelerationStructure*)(ref.Raw());
            as_ptrs[i] = as ? as->GetAccelerationStructure() : nullptr;
        }
        updates->setAccelerationStructureCount(num_slots);
        updates->setPAccelerationStructures(as_ptrs);
        descriptor_write.setPNext(updates);
    } else {
        MI_WARN("Unknown bindless resource type");
    }
    auto ptr_set_index = &set_index_;
    auto ptr_sets = bindless_descriptor_sets_;
    EnqueueRHIThreadTask([descriptor_write, ptr_set_index, ptr_sets]() {
        auto device = GetVulkanRHI()->GetDevice();
        auto curr_write = descriptor_write;
        curr_write.dstSet = ptr_sets[*ptr_set_index];
        device.updateDescriptorSets({curr_write}, {});
    });
}

void VulkanBindlessManager::AdvanceFrame_RHIThread (std::span<RHIPackedBindlessSlot> slots_to_free) {
    // Free batched slots
    if (!slots_to_free.empty()) {
        auto device = GetVulkanRHI()->GetDevice();
        std::vector<vk::WriteDescriptorSet> null_descriptors;
        std::vector<vk::DescriptorBufferInfo> null_buffers;
        std::vector<vk::DescriptorImageInfo> null_images;
        std::vector<vk::WriteDescriptorSetAccelerationStructureKHR> null_acceleration_structures;
        null_descriptors.reserve(slots_to_free.size());
        int num_null_buffers = 0, num_null_images = 0, num_null_acceleration_structures = 0;
        for (auto & slot : slots_to_free) {
            auto type = (RHIBindlessResourceType)slot.type;
            auto slot_index = slot.slot_index;
            {
                auto vk_type = GetVulkanDescriptorType(type);
                auto null_descriptor = vk::WriteDescriptorSet {
                    bindless_descriptor_sets_[set_index_],
                    static_cast<uint32_t>(type),
                    slot_index,
                    1,
                    vk_type
                };
                if (type == RHIBindlessResourceType::kReadOnlyStorageBuffer) {
                    num_null_buffers ++;
                } else if (type == RHIBindlessResourceType::kSRV) {
                    num_null_images ++;
                } else if (type == RHIBindlessResourceType::kAccelerationStructure) {
                    num_null_acceleration_structures ++;
                }
                null_descriptors.push_back(null_descriptor);
            }
        }
        null_buffers.reserve(num_null_buffers);
        null_images.reserve(num_null_images);
        null_acceleration_structures.reserve(num_null_acceleration_structures);
        for (auto [slot, desc] : std::views::zip(slots_to_free, null_descriptors)) {
            auto type = slot.type;
            if (type == RHIBindlessResourceType::kReadOnlyStorageBuffer) {
                auto null_buffer = vk::DescriptorBufferInfo {
                    nullptr,
                    0,
                    VK_WHOLE_SIZE
                };
                null_buffers.push_back(null_buffer);
                desc.setPBufferInfo(null_buffers.data());
            } else if (type == RHIBindlessResourceType::kSRV) {
                auto null_image = vk::DescriptorImageInfo {
                    nullptr,
                    nullptr,
                    vk::ImageLayout::eUndefined
                };
                null_images.push_back(null_image);
                desc.setPImageInfo(null_images.data());
            } else if (type == RHIBindlessResourceType::kAccelerationStructure) {
                auto null_acceleration_structure = vk::WriteDescriptorSetAccelerationStructureKHR {
                    1,
                    nullptr
                };
                null_acceleration_structures.push_back(null_acceleration_structure);
                desc.setPNext(&null_acceleration_structures.back());
            }
        }
        device.updateDescriptorSets(null_descriptors, {});
        // Remember to free the memory allocated by PrepareDelayedSlotsForRHIFree
        delete [] slots_to_free.data();
    }

    set_index_ = (set_index_ + 1) % 2;
    auto device = GetVulkanRHI()->GetDevice();
    // Copy the previous set to the new set
    std::vector<vk::CopyDescriptorSet> copies;
    for (int i = 0; i < std::size(bindless_channels_); i++) {
        auto size = bindless_channels_[i].total_count;
        auto copy = vk::CopyDescriptorSet {
            bindless_descriptor_sets_[set_index_ ^ 1],
            (uint32_t)i,
            0,
            bindless_descriptor_sets_[set_index_],
            (uint32_t)i,
            0,
            (uint32_t)size
        };
        copies.push_back(copy);
    }
    device.updateDescriptorSets({}, copies);
}

MI_NAMESPACE_END