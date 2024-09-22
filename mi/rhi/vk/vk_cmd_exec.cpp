/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_rhi.h"
#include "vk_cmd_exec.h"
#include "vk_resource.h"
#include "vk_buffer.h"
#include "vk_texture.h"
#include "vk_pipeline.h"
#include "vk_conversion.h"
#include "vk_bindless.h"

MI_NAMESPACE_BEGIN

VulkanCommandExecutor::VulkanCommandExecutor() {
    auto fut = EnqueueRHIThreadTask([this](){Initialize_RHIThread();});
    fut.wait();
}

VulkanCommandExecutor::~VulkanCommandExecutor() {
    auto fut = EnqueueRHIThreadTask([this](){Destroy_RHIThread();});
    fut.wait();
}

void VulkanCommandExecutor::Initialize_RHIThread() {
    assert(IsRHIThread());
    for(int i = 0; i < (int)RHICommandQueueType::kMax; i++) {
        auto & chain = state_chains_[i];
        for(auto & state : chain.states) {
            state.Init((RHICommandQueueType)i);
        }
    }
}

void VulkanCommandExecutor::Destroy_RHIThread() {
    assert(IsRHIThread());
    for(int i = 0; i < (int)RHICommandQueueType::kMax; i++) {
        auto & chain = state_chains_[i];
        for(auto & state : chain.states) {
            state.Destroy();
        }
    }
}

template<typename T> concept VKLayoutType = std::is_same_v<T, vk::ImageLayout>;
template<VKLayoutType...Layouts>
static void CheckImageLayout(VulkanTexture * texture, [[maybe_unused]] Layouts...layout) {
#ifndef NDEBUG
    auto current_layout = texture->GetImageLayout();
    bool valid = ((current_layout == layout) || ...);
    auto expected = (vk::to_string(vk::ImageLayout(layout)) + ...);
    auto current = vk::to_string(current_layout);
    mi_assert(valid, "Invalid image layout. Expected: {}, Current: {}.", expected, current);
#endif
}

void VulkanCommandExecutor::RHIClearTexture(RHICommandQueueBase *cmd, RHICommandClearTexture *clear_texture) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto texture = static_cast<VulkanTexture*>(clear_texture->texture_);
    auto & region = vk::ImageSubresourceRange()
            .setAspectMask(texture->GetImageAspect())
            .setBaseMipLevel(clear_texture->mip_level_)
            .setLevelCount(1)
            .setBaseArrayLayer(clear_texture->base_layer_)
            .setLayerCount(clear_texture->layer_count_);
    auto & color = clear_texture->clear_value_;
    CheckImageLayout(texture, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
    state.cmd.clearColorImage(texture->GetImage(), texture->GetImageLayout(), vk::ClearColorValue(color), region);
}

void VulkanCommandExecutor::RHICopyBufferToTexture(RHICommandQueueBase *cmd,
                                                   RHICommandCopyBufferToTexture *copy_buffer_to_texture) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;
    auto src_buffer = static_cast<VulkanBuffer*>(copy_buffer_to_texture->buffer_.buffer);
    auto dst_texture = static_cast<VulkanTexture*>(copy_buffer_to_texture->texture_);
    auto & region = vk::BufferImageCopy()
            .setBufferOffset(copy_buffer_to_texture->buffer_.offset)
            .setBufferRowLength(copy_buffer_to_texture->src_tex_width_)
            .setBufferImageHeight(copy_buffer_to_texture->src_tex_height_)
            .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(dst_texture->GetImageAspect())
                    .setMipLevel(copy_buffer_to_texture->mip_level_)
                    .setBaseArrayLayer(copy_buffer_to_texture->base_layer_)
                    .setLayerCount(copy_buffer_to_texture->layer_count_)
            )
            .setImageOffset({copy_buffer_to_texture->dst_tex_x_, copy_buffer_to_texture->dst_tex_y_, copy_buffer_to_texture->dst_tex_z_})
            .setImageExtent({copy_buffer_to_texture->dst_tex_width_, copy_buffer_to_texture->dst_tex_height_, copy_buffer_to_texture->dst_tex_depth_});
    CheckImageLayout(dst_texture, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
    cmdb.copyBufferToImage(src_buffer->GetBuffer(), dst_texture->GetImage(), dst_texture->GetImageLayout(), region);
}

void VulkanCommandExecutor::RHICopyTextureToBuffer(RHICommandQueueBase *cmd,
                                                   RHICommandCopyTextureToBuffer *copy_texture_to_buffer) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;
    auto src_texture = static_cast<VulkanTexture*>(copy_texture_to_buffer->texture_);
    auto dst_buffer = static_cast<VulkanBuffer*>(copy_texture_to_buffer->buffer_.buffer);
    auto & region = vk::BufferImageCopy()
            .setBufferOffset(copy_texture_to_buffer->buffer_.offset)
            .setBufferRowLength(copy_texture_to_buffer->dst_tex_width_)
            .setBufferImageHeight(copy_texture_to_buffer->dst_tex_height_)
            .setImageSubresource(vk::ImageSubresourceLayers()
                    .setAspectMask(src_texture->GetImageAspect())
                    .setMipLevel(copy_texture_to_buffer->mip_level_)
                    .setBaseArrayLayer(copy_texture_to_buffer->base_layer_)
                    .setLayerCount(copy_texture_to_buffer->layer_count_)
            )
            .setImageOffset({copy_texture_to_buffer->src_tex_x_, copy_texture_to_buffer->src_tex_y_, copy_texture_to_buffer->src_tex_z_})
            .setImageExtent({copy_texture_to_buffer->src_tex_width_, copy_texture_to_buffer->src_tex_height_, copy_texture_to_buffer->src_tex_depth_});
    CheckImageLayout(src_texture, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral);
    cmdb.copyImageToBuffer(src_texture->GetImage(), src_texture->GetImageLayout(), dst_buffer->GetBuffer(), region);
}

void VulkanCommandExecutor::RHICopyBuffer(RHICommandQueueBase *queue, RHICommandCopyBuffer *copy_buffer) {
    assert(IsRHIThread());
    auto & cmd = state_chains_[(uint32_t)queue->GetCommandQueueType()].Current().cmd;
    auto & region = vk::BufferCopy()
        .setSrcOffset(copy_buffer->src_.offset)
        .setDstOffset(copy_buffer->dst_.offset)
        .setSize(std::min(copy_buffer->src_.size, copy_buffer->dst_.size));
    auto * src_buffer = static_cast<VulkanBuffer*>(copy_buffer->src_.buffer);
    auto * dst_buffer = static_cast<VulkanBuffer*>(copy_buffer->dst_.buffer);
    cmd.copyBuffer(src_buffer->GetBuffer(),dst_buffer->GetBuffer(),region);
}

void VulkanCommandExecutor::RHICopyTexture(RHICommandQueueBase *queue, RHICommandCopyTexture *copy_texture) {
    assert(IsRHIThread());
    auto & cmd = state_chains_[(uint32_t)queue->GetCommandQueueType()].Current().cmd;
    auto src_texture = static_cast<VulkanTexture*>(copy_texture->src_);
    auto dst_texture = static_cast<VulkanTexture*>(copy_texture->dst_);
    auto region = vk::ImageCopy2()
        .setSrcSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(copy_texture->src_mip_)
            .setBaseArrayLayer(copy_texture->src_base_layer_)
            .setLayerCount(copy_texture->src_layer_count_))
        .setDstSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(vk::ImageAspectFlagBits::eColor)
            .setMipLevel(copy_texture->dst_mip_)
            .setBaseArrayLayer(copy_texture->dst_base_layer_)
            .setLayerCount(copy_texture->dst_layer_count_))
        .setSrcOffset({copy_texture->src_x_, copy_texture->src_y_, copy_texture->src_z_})
        .setDstOffset({copy_texture->dst_x_, copy_texture->dst_y_, copy_texture->dst_z_})
        .setExtent({copy_texture->width_, copy_texture->height_, copy_texture->depth_});
    auto & copy_info = vk::CopyImageInfo2()
            .setSrcImage(src_texture->GetImage())
            .setDstImage(dst_texture->GetImage())
            .setSrcImageLayout(src_texture->GetImageLayout())
            .setDstImageLayout(dst_texture->GetImageLayout())
            .setRegions(region);
    CheckImageLayout(src_texture, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral);
    CheckImageLayout(dst_texture, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
    cmd.copyImage2(copy_info);
}

const static vk::PipelineStageFlags kBasicDrawStages = vk::PipelineStageFlagBits::eGeometryShader
    | vk::PipelineStageFlagBits::eVertexInput | vk::PipelineStageFlagBits::eVertexShader
    | vk::PipelineStageFlagBits::eTessellationControlShader | vk::PipelineStageFlagBits::eTessellationEvaluationShader
    | vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests
    | vk::PipelineStageFlagBits::eFragmentShader;

void VulkanCommandExecutor::RHIDrawPrimitive(RHICommandQueueBase *cmd,
                                             RHICommandDrawPrimitive *draw_primitive) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

//    auto & point = state.points[(uint32_t)RHIBindPointType::kGraphics];
    FlushBindPointDescriptorWrites(cmd, RHIBindPointType::kGraphics, kBasicDrawStages);

//    for(auto & vb : state.bound_vertex_buffers) {
//        if(!vb.IsValid()) continue;
//        auto * buffer = static_cast<VulkanBuffer*>(vb.buffer); // NOLINT its safe
//        buffer->Use(state.cmd, vk::PipelineStageFlagBits::eVertexInput, vk::AccessFlagBits::eVertexAttributeRead);
//    }
    state.cmd.draw(draw_primitive->vertex_count_, draw_primitive->instance_count_, draw_primitive->first_vertex_, draw_primitive->first_instance_);
}

void VulkanCommandExecutor::RHIDrawIndexedPrimitive(RHICommandQueueBase *cmd,
                                                    RHICommandDrawIndexedPrimitive *draw_indexed_primitive) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

//    auto & point = state.points[(uint32_t)RHIBindPointType::kGraphics];
    FlushBindPointDescriptorWrites(cmd, RHIBindPointType::kGraphics, kBasicDrawStages);

//    for(auto & vb : state.bound_vertex_buffers) {
//        if(!vb.IsValid()) continue;
//        auto * buffer = static_cast<VulkanBuffer*>(vb.buffer); // NOLINT its safe
//        buffer->Use(state.cmd, vk::PipelineStageFlagBits::eVertexInput, vk::AccessFlagBits::eVertexAttributeRead);
//    }
    auto index_buffer = static_cast<VulkanBuffer*>(draw_indexed_primitive->index_buffer_.buffer); // NOLINT its safe
//    index_buffer->Use(state.cmd, vk::PipelineStageFlagBits::eVertexInput, vk::AccessFlagBits::eIndexRead);
    state.cmd.bindIndexBuffer(index_buffer->GetBuffer(), draw_indexed_primitive->index_buffer_.offset, GetVulkanIndexType(draw_indexed_primitive->index_type_));
    state.cmd.drawIndexed(draw_indexed_primitive->index_count_,
                          draw_indexed_primitive->instance_count_,
                          draw_indexed_primitive->first_index_,
                          draw_indexed_primitive->base_vertex_index_,
                          draw_indexed_primitive->first_instance_index_);
}

void VulkanCommandExecutor::RHIDispatch(RHICommandQueueBase *cmd, RHICommandDispatch *dispatch) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
//    auto & point = state.points[(uint32_t)RHIBindPointType::kCompute];
    FlushBindPointDescriptorWrites(cmd, RHIBindPointType::kCompute, vk::PipelineStageFlagBits::eComputeShader);
    state.cmd.dispatch(dispatch->group_count_x_, dispatch->group_count_y_, dispatch->group_count_z_);
}

void VulkanCommandExecutor::RHIBindGraphicsPipeline(RHICommandQueueBase *cmd,
                                                    RHICommandBindGraphicsPipeline *bind_graphics_pipeline) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto pipeline = static_cast<VulkanGraphicsPipeline*>(bind_graphics_pipeline->pipeline_); // NOLINT its safe
    auto & point = state.points[(uint32_t)RHIBindPointType::kGraphics];
    if(point.bound_pipeline != pipeline) {
        point.bound_private_set = nullptr;
        auto set_layout = pipeline->GetPrivateDescriptorSetLayout();
        if(set_layout) {
            auto descriptor_set = GetVulkanRHI()->GetDevice().allocateDescriptorSets(
                    vk::DescriptorSetAllocateInfo()
                            .setDescriptorPool(state.descriptor_pool)
                            .setDescriptorSetCount(1)
                            .setSetLayouts(set_layout)
            );
            mi_assert(!descriptor_set.empty(), "Failed to allocate descriptor set");
            point.bound_private_set = descriptor_set[0];
        }
        state.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->GetPipeline());
        point.bound_pipeline = pipeline;
    }
}

void VulkanCommandExecutor::RHIBindComputePipeline(
        RHICommandQueueBase *cmd, RHICommandBindComputePipeline *bind_compute_pipeline) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto pipeline = static_cast<VulkanComputePipeline*>(bind_compute_pipeline->pipeline_); // NOLINT its safe
    auto & point = state.points[(uint32_t)RHIBindPointType::kCompute];
    if(point.bound_pipeline != pipeline) {
        point.bound_private_set = nullptr;
        auto set_layout = pipeline->GetPrivateDescriptorSetLayout();
        if(set_layout) {
            auto descriptor_set = GetVulkanRHI()->GetDevice().allocateDescriptorSets(
                    vk::DescriptorSetAllocateInfo()
                            .setDescriptorPool(state.descriptor_pool)
                            .setDescriptorSetCount(1)
                            .setSetLayouts(set_layout)
            );
            mi_assert(!descriptor_set.empty(), "Failed to allocate descriptor set");
            point.bound_private_set = descriptor_set[0];
        }
        state.cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->GetPipeline());
        state.points[(uint32_t)RHIBindPointType::kCompute].bound_pipeline = pipeline;
    }
}

void VulkanCommandExecutor::RHIBindRenderTarget(RHICommandQueueBase *cmd,
                                                RHICommandBindRenderTarget *bind_render_target) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & point = state.points[(uint32_t)RHIBindPointType::kGraphics];
    auto & rt = bind_render_target->target_;
    asfasdfasdfdsdf

}

void VulkanCommandExecutor::RHIBindPipelineParameters(
        RHICommandQueueBase *cmd, RHICommandBindPipelineParameters *bind_pipeline_parameters) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto table = bind_pipeline_parameters->table_;
    auto & point = state.points[(uint32_t)bind_pipeline_parameters->point_];
    point.parameter_table.Merge(table);
}

void VulkanCommandExecutor::RHIBindVertexBuffer(RHICommandQueueBase *cmd,
                                               RHICommandBindVertexBuffer *bind_vertex_buffer) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & vb = bind_vertex_buffer->buffer_;
    auto * buffer = static_cast<VulkanBuffer*>(vb.buffer); // NOLINT its safe
    state.cmd.bindVertexBuffers(bind_vertex_buffer->binding_, buffer->GetBuffer(), vb.offset);
    mi_assert(std::size(state.bound_vertex_buffers) > bind_vertex_buffer->binding_, "Invalid binding");
    state.bound_vertex_buffers[bind_vertex_buffer->binding_] = vb;
}

void VulkanCommandExecutor::RHIFrameEnd(RHICommandQueueBase *cmd, RHICommandFrameEnd *frame_end) {
    assert(IsRHIThread());
    auto & chain = state_chains_[(uint32_t)cmd->GetCommandQueueType()];
    {
        // Release resources allocated for the frame before current frame
        int prev_state_index = (chain.state_index - 1) % (int) std::size(chain.states);
        auto &prev_state = chain.states[prev_state_index];
        prev_state.Clear(frame_end->return_resources_to_system_);
        // Switch to the next state
        chain.state_index = (chain.state_index + 1) % (int) std::size(chain.states);
    }
}

void VulkanCommandExecutor::CommandQueueState::BindPoints::ParameterTable::Merge (const RHIBindPipelineParametersDesc * desc) {
    assert(IsRHIThread());
    // Simply append all bindings
    uniforms.insert(uniforms.end(), desc->uniforms.begin(), desc->uniforms.end());
    storages.insert(storages.end(), desc->storages.begin(), desc->storages.end());
    uavs.insert(uavs.end(), desc->uavs.begin(), desc->uavs.end());
    srvs.insert(srvs.end(), desc->srvs.begin(), desc->srvs.end());
    samplers.insert(samplers.end(), desc->samplers.begin(), desc->samplers.end());
    acceleration_structures.insert(acceleration_structures.end(), desc->acceleration_structures.begin(), desc->acceleration_structures.end());
    bindless_resources.insert(bindless_resources.end(), desc->bindless_resources.begin(), desc->bindless_resources.end());
    // Overwrite push constants if any
    if(!desc->constants.empty()) {
        push_constants = desc->constants;
    }
}

// TODO remove the [[maybe_unused]] stuff.
VulkanCommandExecutor::DescriptorWrites
VulkanCommandExecutor::CommandQueueState::BindPoints::ParameterTable::FlushDescriptorWrites(
    RHICommandQueueBase * cmd, [[maybe_unused]] vk::Device device, vk::DescriptorSet descriptor_set, std::span<std::uint32_t> btb_data,
    [[maybe_unused]] vk::CommandBuffer cmdb, [[maybe_unused]] vk::PipelineStageFlags use_stages
) {
    assert(IsRHIThread());
    // Sort and merge all recorded bindings
    auto SortUnique = [&](auto & arr) {
        std::stable_sort(arr.begin(), arr.end(), [](const auto & a, const auto & b) {
            return a.binding < b.binding;
        });
        std::reverse(arr.begin(), arr.end());
        auto tail = std::unique(arr.begin(), arr.end(), [](const auto & a, const auto & b) {
            return a.binding == b.binding;
        });
        arr.erase(tail, arr.end());
    };
    SortUnique(uniforms);
    SortUnique(storages);
    SortUnique(uavs);
    SortUnique(srvs);
    SortUnique(samplers);
    SortUnique(acceleration_structures);

    // Count all writes that needed to allocate a WriteDescriptorSet array
    uint32_t write_count = (uint32_t)
            (uniforms.size() + storages.size() + uavs.size() + srvs.size() + samplers.size() + acceleration_structures.size());
    vk::WriteDescriptorSet * writes = cmd->Allocate<vk::WriteDescriptorSet[]>(write_count);
    int write_index = 0;
    for(auto ubo : uniforms) {
        auto& buffer_info = *cmd->Allocate<vk::DescriptorBufferInfo>();
        buffer_info.buffer = static_cast<VulkanBuffer*>(ubo.buffer.buffer)->GetBuffer(); // NOLINT its safe
        buffer_info.offset = ubo.buffer.offset;
        buffer_info.range = ubo.buffer.size;
//        auto * buffer = static_cast<VulkanBuffer*>(ubo.buffer.buffer); // NOLINT its safe
//        buffer->Use(cmdb, use_stages, vk::AccessFlagBits::eUniformRead);
        auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(ubo.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setPBufferInfo(&buffer_info);
        writes[write_index++] = write;
    }
    for(auto storage : storages) {
        auto& buffer_info = *cmd->Allocate<vk::DescriptorBufferInfo>();
        auto buffer = static_cast<VulkanBuffer*>(storage.buffer.buffer); // NOLINT its safe
        buffer_info.buffer = buffer->GetBuffer(); // NOLINT its safe
        buffer_info.offset = storage.buffer.offset;
        buffer_info.range = storage.buffer.size;
//        buffer->Use(cmdb, use_stages, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(storage.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                .setPBufferInfo(&buffer_info);
        writes[write_index++] = write;
    }
    for(auto uav : uavs) {
        auto& image_info = *cmd->Allocate<vk::DescriptorImageInfo>();
        auto image = static_cast<VulkanTexture*>(uav.texture);
        image_info.imageView = image->GetImageView(); // NOLINT its safe
        image_info.imageLayout = vk::ImageLayout::eGeneral;
//        image->Use(cmdb, vk::ImageLayout::eGeneral, use_stages, vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite);
        auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(uav.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageImage)
                .setPImageInfo(&image_info);
        writes[write_index++] = write;
    }
    for(auto srv : srvs) {
        auto& image_info = *cmd->Allocate<vk::DescriptorImageInfo>();
        auto image = static_cast<VulkanTexture*>(srv.texture);
        image_info.imageView = image->GetImageView(); // NOLINT its safe
        image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
//        image->Use(cmdb, vk::ImageLayout::eShaderReadOnlyOptimal, use_stages, vk::AccessFlagBits::eShaderRead);
        auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(srv.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eSampledImage)
                .setPImageInfo(&image_info);
        writes[write_index++] = write;
    }
    for(auto sampler : samplers) {
        auto& image_info = *cmd->Allocate<vk::DescriptorImageInfo>();
        image_info.sampler = static_cast<VulkanSampler*>(sampler.resource)->GetSampler(); // NOLINT its safe
        auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(sampler.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eSampler)
                .setPImageInfo(&image_info);
        writes[write_index++] = write;
    }
    for(auto acc : acceleration_structures) {
        auto& write_khr = *cmd->Allocate<vk::WriteDescriptorSetAccelerationStructureKHR>();
        auto  p_ac = cmd->Allocate<vk::AccelerationStructureKHR>();
        auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(acc.binding)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eAccelerationStructureKHR)
                .setPNext(&write_khr);
        write_khr.accelerationStructureCount = 1;
        auto rhi_acc = static_cast<VulkanAccelerationStructure*>(acc.resource);
//        rhi_acc->Use(cmdb, use_stages, vk::AccessFlagBits::eAccelerationStructureReadKHR);
        *(vk::AccelerationStructureKHR*)p_ac = (rhi_acc->GetAccelerationStructure()); // NOLINT its safe
        write_khr.pAccelerationStructures = p_ac;
        writes[write_index++] = write;
    }
    // Clear all bindings
    uniforms.clear();
    storages.clear();
    uavs.clear();
    srvs.clear();
    samplers.clear();
    acceleration_structures.clear();

    // Generate btb table data for bindless resources
    SortUnique(bindless_resources);
//    auto vk_rhi = GetVulkanRHI();
//    auto vk_bindless_mgr = vk_rhi->GetVulkanBindlessManager();

    // The user should manage bindless texture layouts manually.
    for(auto & bindless : bindless_resources) {
        int bindless_binding = bindless.binding;
        int bindless_slot    = bindless.bindless_slot;
        btb_data[bindless_binding] = bindless_slot;
    }

    // Clear bindless resources
    bindless_resources.clear();

    return std::span<vk::WriteDescriptorSet>(writes, write_count);
}

void VulkanCommandExecutor::FlushBindPointDescriptorWrites(
        RHICommandQueueBase * cmd, RHIBindPointType point_t, vk::PipelineStageFlags use_stages) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & point = state.points[(uint32_t)point_t];
    // Assign btb on the fly
    uint32_t btb_size = RoundUp(
            point.bound_pipeline->GetBindlessTableSize(),
            GetVulkanRHI()->QueryRHIBindlessSupportInfo().descriptor_buffer_offset_alignment
    );
    std::span<uint32_t> btb_data = {
            (uint32_t*)((std::byte*)point.bindless_table_buffer->Map() + point.bindless_table_top),
            btb_size / sizeof(uint32_t)
    };
    point.bindless_table_top += btb_size;
    auto descriptor_writes = point.parameter_table.FlushDescriptorWrites(
            cmd, GetVulkanRHI()->GetDevice(), point.bound_private_set, btb_data,
            state.cmd, use_stages
    );
    if(!descriptor_writes.empty()) {
        GetVulkanRHI()->GetDevice().updateDescriptorSets(descriptor_writes, {});
    }
}

void VulkanCommandExecutor::RHITextureBarrier(RHICommandQueueBase *cmd,
                                              RHICommandTextureBarrier *barrier) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto texture = static_cast<VulkanTexture*>(barrier->texture_);
    texture->Barrier(state.cmd,
                     GetVulkanImageLayout(barrier->layout_),
                     GetVulkanPipelineStageFlags(barrier->src_stages_),
                     GetVulkanPipelineStageFlags(barrier->dst_stages_),
                     GetVulkanAccessFlags(barrier->src_access_),
                     GetVulkanAccessFlags(barrier->dst_access_)
    );
}

void
VulkanCommandExecutor::RHIBufferBarrier(RHICommandQueueBase *cmd, RHICommandBufferBarrier *barrier) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto buffer = static_cast<VulkanBuffer*>(barrier->buffer_.buffer);
    buffer->MemBarrier(state.cmd,
                       GetVulkanPipelineStageFlags(barrier->src_stages_),
                       GetVulkanPipelineStageFlags(barrier->dst_stages_),
                       GetVulkanAccessFlags(barrier->src_access_),
                       GetVulkanAccessFlags(barrier->dst_access_),
                       barrier->buffer_.offset,
                       barrier->buffer_.size
    );
}

void
VulkanCommandExecutor::RHISubmitCommandBuffer(RHICommandQueueBase *buffer, RHISyncPoint * sync, bool recycle_resources) {
    assert(IsRHIThread());
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current();
    auto & cmd = state.cmd;
    cmd.end();
    auto vk_rhi = GetVulkanRHI();
    auto queue = vk_rhi->GetQueue(buffer->GetCommandQueueType());
    auto submit_info = vk::SubmitInfo()
            .setCommandBufferCount(1)
            .setPCommandBuffers(&cmd);
    queue.submit(submit_info, ((VulkanSyncPoint*)sync)->GetFence());
    cmd.reset(recycle_resources ? vk::CommandBufferResetFlagBits::eReleaseResources : vk::CommandBufferResetFlagBits{});
}

void VulkanCommandExecutor::CommandQueueState::Init(RHICommandQueueType type) {
    assert(IsRHIThread());
    {
        auto rhi = GetVulkanRHI();
        cmd_pool = rhi->GetDevice().createCommandPool(
                vk::CommandPoolCreateInfo{
                        vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                        rhi->GetQueueFamilyIndex(type)
                }
        );
        cmd = rhi->GetDevice().allocateCommandBuffers(
                vk::CommandBufferAllocateInfo{
                        cmd_pool,
                        vk::CommandBufferLevel::ePrimary,
                        1
                }
        )[0];
        for(auto & span = bound_vertex_buffers; auto & buf : span)
            buf = RHIBufferSpan{};
        for(auto & point : points) {
            point.bindless_table_buffer = nullptr;
            point.bindless_table_top = 0;
            point.bound_private_set = nullptr;
            point.bound_pipeline = nullptr;
            point.parameter_table = {};
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
                        vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
                        C::kMaxNumDescriptorSetsPerFrame,
                        pool_sizes
                }
        );
    }
}

void VulkanCommandExecutor::CommandQueueState::Destroy() {
    assert(IsRHIThread());
    auto rhi = GetVulkanRHI();
    for(auto & point : points) {
        if(point.bindless_table_buffer) {
            point.bindless_table_buffer->Unmap();
            point.bindless_table_buffer = {};
        }
    }
    rhi->GetDevice().destroyDescriptorPool(descriptor_pool);
    rhi->GetDevice().freeCommandBuffers(cmd_pool, cmd);
    rhi->GetDevice().destroyCommandPool(cmd_pool);
}

void VulkanCommandExecutor::CommandQueueState::Clear(bool return_resources_to_system) {
    assert(IsRHIThread());
    auto rhi = GetVulkanRHI();
    for(auto & point : points) {
        if(point.bound_private_set) {
            rhi->GetDevice().freeDescriptorSets(descriptor_pool, point.bound_private_set);
            point.bound_private_set = nullptr;
        }
        point.bound_pipeline = nullptr;
        point.parameter_table = {};
    }
    // Do not recycle resources back to the system. We may be able to reuse them in the later frames.
    rhi->GetDevice().resetCommandPool(cmd_pool,
                  return_resources_to_system
                  ? vk::CommandPoolResetFlagBits::eReleaseResources : vk::CommandPoolResetFlagBits{});
    rhi->GetDevice().resetDescriptorPool(descriptor_pool);
}

MI_NAMESPACE_END
