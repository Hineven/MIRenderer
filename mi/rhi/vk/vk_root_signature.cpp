/*
 * Created: 2026/4/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "vk_rhi.h"
#include "vk_root_signature.h"
#include "vk_bindless.h"
#include "core/rounding.h"

MI_NAMESPACE_BEGIN

VulkanRootSignature::VulkanRootSignature(const RHIPipelineRootSignatureDesc & desc, vk::Device device)
    : device_(device)
{
    push_constant_size_ = desc.push_constant_size;
    memcpy(num_resources_, desc.num_resources, sizeof(num_resources_));

    std::vector<vk::DescriptorSetLayout> descriptor_set_layouts;
    std::vector<vk::DescriptorSetLayoutBinding> bindfull_bindings;
    uint32_t current_binding = 0;

    struct TypeDesc {
        RHIPipelineResourceType rhi_type;
        vk::DescriptorType vk_type;
        uint32_t count;
    };
    TypeDesc types[] = {
        {RHIPipelineResourceType::kUniformBuffer,         vk::DescriptorType::eUniformBuffer,         desc.num_resources[(uint32_t)RHIPipelineResourceType::kUniformBuffer]},
        {RHIPipelineResourceType::kStorageBuffer,         vk::DescriptorType::eStorageBuffer,         desc.num_resources[(uint32_t)RHIPipelineResourceType::kStorageBuffer]},
        {RHIPipelineResourceType::kUAV,                   vk::DescriptorType::eStorageImage,          desc.num_resources[(uint32_t)RHIPipelineResourceType::kUAV]},
        {RHIPipelineResourceType::kSRV,                   vk::DescriptorType::eSampledImage,          desc.num_resources[(uint32_t)RHIPipelineResourceType::kSRV]},
        {RHIPipelineResourceType::kSampler,               vk::DescriptorType::eSampler,               desc.num_resources[(uint32_t)RHIPipelineResourceType::kSampler]},
        {RHIPipelineResourceType::kAccelerationStructure, vk::DescriptorType::eAccelerationStructureKHR, desc.num_resources[(uint32_t)RHIPipelineResourceType::kAccelerationStructure]},
    };

    for (auto & td : types) {
        binding_base_[(uint32_t)td.rhi_type] = current_binding;
        for (uint32_t i = 0; i < td.count; i++) {
            bindfull_bindings.emplace_back()
                .setBinding(current_binding)
                .setDescriptorType(td.vk_type)
                .setDescriptorCount(1)
                .setStageFlags(vk::ShaderStageFlagBits::eAll);
            remappings_.AddRemapping(td.rhi_type, i, 0, current_binding);
            current_binding++;
        }
    }

    // Copy CRC data into owned storage and set up type_names_
    size_t total_crcs = 0;
    for (uint32_t t = 0; t < (uint32_t)RHIPipelineResourceType::kMax; t++) {
        total_crcs += desc.type_names[t].count;
    }
    if (total_crcs > 0) {
        owned_crc_data_.resize(total_crcs);
        uint32_t * dst = owned_crc_data_.data();
        for (uint32_t t = 0; t < (uint32_t)RHIPipelineResourceType::kMax; t++) {
            auto & src = desc.type_names[t];
            type_names_[t].count = src.count;
            if (src.count > 0 && src.name_crcs) {
                memcpy(dst, src.name_crcs, src.count * sizeof(uint32_t));
                type_names_[t].name_crcs = dst;
                dst += src.count;
            }
        }
    }

    // Set 0: bindfull descriptor set layout (always created, even with 0 bindings)
    descriptor_set_layout_ = device.createDescriptorSetLayout(
        vk::DescriptorSetLayoutCreateInfo()
            .setBindingCount((uint32_t)bindfull_bindings.size())
            .setPBindings(bindfull_bindings.data())
    );
    descriptor_set_layouts.push_back(descriptor_set_layout_);

    // Set 1: bindless descriptor set layout (always present)
    auto & bindless_manager = *GetVulkanRHI()->GetVulkanBindlessManager();
    descriptor_set_layouts.push_back(bindless_manager.GetBindlessDescriptorSetLayout());

    uint32_t push_constant_roundup = RoundUp(push_constant_size_, 128);
    auto info = vk::PipelineLayoutCreateInfo{}
        .setSetLayoutCount((uint32_t)descriptor_set_layouts.size())
        .setPSetLayouts(descriptor_set_layouts.data());
    // NOTE: `pc` must outlive the createPipelineLayout() call below. Vulkan-Hpp's
    // setPushConstantRanges() stores a POINTER to the range (ArrayProxy does not copy), so
    // declaring it inside the if-block would leave pPushConstantRanges dangling by the time
    // createPipelineLayout reads it — the validation layer then reports garbage stageFlags
    // (leftover stack bytes) and the push-constant range is effectively lost (black screen).
    vk::PushConstantRange pc;
    pc.setOffset(0);
    pc.setSize(push_constant_roundup);
    pc.setStageFlags(vk::ShaderStageFlagBits::eAll);
    if (push_constant_roundup > 0) {
        info.setPushConstantRanges(pc);
    }
    pipeline_layout_ = device.createPipelineLayout(info);
}

VulkanRootSignature::~VulkanRootSignature() {
    if (pipeline_layout_) {
        device_.destroy(pipeline_layout_);
    }
    if (descriptor_set_layout_) {
        device_.destroy(descriptor_set_layout_);
    }
}

void *VulkanRootSignature::GetAPIHandle() const {
    return (void*)pipeline_layout_;
}

MI_NAMESPACE_END
