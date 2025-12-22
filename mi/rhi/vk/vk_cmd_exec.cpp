/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "vk_rhi.h"
#include "vk_cmd_exec.h"
#include "vk_resource.h"
#include "vk_as.h"
#include "vk_buffer.h"
#include "vk_texture.h"
#include "vk_pipeline.h"
#include "vk_conversion.h"
#include "vk_bindless.h"
#include "core/util/debug_prof.h"

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
    CHECK_RHI_THREAD();
    for(int i = 0; i < (int)RHICommandQueueType::kMax; i++) {
        auto & chain = state_chains_[i];
        for(auto & state : chain.states) {
            state.Init((RHICommandQueueType)i);
        }
    }
}

void VulkanCommandExecutor::Destroy_RHIThread() {
    CHECK_RHI_THREAD();
    for(int i = 0; i < (int)RHICommandQueueType::kMax; i++) {
        auto & chain = state_chains_[i];
        for(auto & state : chain.states) {
            state.Destroy();
        }
    }
}

template<typename T> concept VKLayoutType = std::is_same_v<T, vk::ImageLayout>;
template<VKLayoutType...Layouts>
static void CheckImageLayout([[maybe_unused]] VulkanTexture * texture, [[maybe_unused]] Layouts...layout) {
#ifndef NDEBUG
    auto current_layout = texture->GetImageLayout();
    bool valid = ((current_layout == layout) || ...);
    auto expected = (vk::to_string(vk::ImageLayout(layout)) + ...);
    auto current = vk::to_string(current_layout);
    mi_assert(valid, "Invalid image layout. Expected: {}, Current: {}.", expected, current);
#endif
}

void VulkanCommandExecutor::RHIClearTexture(RHICommandQueueBase *cmd, RHICommandClearTexture *clear_texture) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto texture = static_cast<VulkanTexture*>(clear_texture->texture_);
    auto region = vk::ImageSubresourceRange()
            .setAspectMask(texture->GetImageAspect())
            .setBaseMipLevel(clear_texture->mip_level_)
            .setLevelCount(1)
            .setBaseArrayLayer(clear_texture->base_layer_)
            .setLayerCount(clear_texture->layer_count_);
    auto & color = clear_texture->clear_value_;
    CheckImageLayout(texture, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
    if (texture->GetImageAspect() & vk::ImageAspectFlagBits::eColor) {
        state.cmd.clearColorImage(texture->GetImage(), texture->GetImageLayout(), vk::ClearColorValue(color), region);
    } else {
        // TODO support stencil clear value.
        state.cmd.clearDepthStencilImage(
            texture->GetImage(), texture->GetImageLayout(),
            vk::ClearDepthStencilValue(color[0], 0),
            region
        );
    }
}

void VulkanCommandExecutor::RHIClearBuffer(RHICommandQueueBase *cmd, RHICommandClearBuffer *clear_buffer) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto buffer = static_cast<VulkanBuffer*>(clear_buffer->buffer_.buffer);
    state.cmd.fillBuffer(
        buffer->GetBuffer(), clear_buffer->buffer_.offset, clear_buffer->buffer_.size,
        clear_buffer->clear_value_
    );
}


void VulkanCommandExecutor::RHICopyBufferToTexture(RHICommandQueueBase *cmd,
                                                   RHICommandCopyBufferToTexture *copy_buffer_to_texture) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;
    auto src_buffer = static_cast<VulkanBuffer*>(copy_buffer_to_texture->buffer_.buffer);
    auto dst_texture = static_cast<VulkanTexture*>(copy_buffer_to_texture->texture_);
    auto region = vk::BufferImageCopy()
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
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & cmdb = state.cmd;
    auto src_texture = static_cast<VulkanTexture*>(copy_texture_to_buffer->texture_);
    auto dst_buffer = static_cast<VulkanBuffer*>(copy_texture_to_buffer->buffer_);
    auto region = vk::BufferImageCopy()
            .setBufferOffset(copy_texture_to_buffer->buffer_offset_)
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
    CHECK_RHI_THREAD();
    auto & cmd = state_chains_[(uint32_t)queue->GetCommandQueueType()].Current().cmd;
    auto region = vk::BufferCopy()
        .setSrcOffset(copy_buffer->src_.offset)
        .setDstOffset(copy_buffer->dst_.offset)
        .setSize(std::min(copy_buffer->src_.size, copy_buffer->dst_.size));
    auto * src_buffer = static_cast<VulkanBuffer*>(copy_buffer->src_.buffer);
    auto * dst_buffer = static_cast<VulkanBuffer*>(copy_buffer->dst_.buffer);
    cmd.copyBuffer(src_buffer->GetBuffer(),dst_buffer->GetBuffer(),region);
}

void VulkanCommandExecutor::RHICopyTexture(RHICommandQueueBase *queue, RHICommandCopyTexture *copy_texture) {
    CHECK_RHI_THREAD();
    auto & cmd = state_chains_[(uint32_t)queue->GetCommandQueueType()].Current().cmd;
    auto src_texture = static_cast<VulkanTexture*>(copy_texture->src_);
    auto dst_texture = static_cast<VulkanTexture*>(copy_texture->dst_);
    auto width = copy_texture->width_ ? copy_texture->width_ : src_texture->GetMipWidth(copy_texture->src_mip_);
    auto height = copy_texture->height_ ? copy_texture->height_ : src_texture->GetMipHeight(copy_texture->src_mip_);
    auto depth = copy_texture->depth_ ? copy_texture->depth_ : src_texture->GetMipDepth(copy_texture->src_mip_);
    auto region = vk::ImageCopy2()
        .setSrcSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(src_texture->GetImageAspect())
            .setMipLevel(copy_texture->src_mip_)
            .setBaseArrayLayer(copy_texture->src_base_layer_)
            .setLayerCount(copy_texture->src_layer_count_))
        .setDstSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(dst_texture->GetImageAspect())
            .setMipLevel(copy_texture->dst_mip_)
            .setBaseArrayLayer(copy_texture->dst_base_layer_)
            .setLayerCount(copy_texture->dst_layer_count_))
        .setSrcOffset({copy_texture->src_x_, copy_texture->src_y_, copy_texture->src_z_})
        .setDstOffset({copy_texture->dst_x_, copy_texture->dst_y_, copy_texture->dst_z_})
        .setExtent({width, height, depth});
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

void VulkanCommandExecutor::RHIBlitTexture(RHICommandQueueBase *queue, RHICommandBlitTexture *blit_texture) {
    CHECK_RHI_THREAD();
    auto & cmd = state_chains_[(uint32_t)queue->GetCommandQueueType()].Current().cmd;
    auto src_texture = static_cast<VulkanTexture*>(blit_texture->src_);
    auto dst_texture = static_cast<VulkanTexture*>(blit_texture->dst_);
    // auto width = blit_texture->width_ ? blit_texture->width_ : src_texture->GetMipWidth(blit_texture->src_mip_);
    // auto height = blit_texture->height_ ? blit_texture->height_ : src_texture->GetMipHeight(blit_texture->src_mip_);
    // auto depth = blit_texture->depth_ ? blit_texture->depth_ : src_texture->GetMipDepth(blit_texture->src_mip_);
    std::array src_offsets = {
        vk::Offset3D{blit_texture->src_x_, blit_texture->src_y_, blit_texture->src_z_},
        vk::Offset3D{blit_texture->src_end_x_, blit_texture->src_end_y_, blit_texture->src_end_z_}
    };
    std::array dst_offsets = {
        vk::Offset3D{blit_texture->dst_x_, blit_texture->dst_y_, blit_texture->dst_z_},
        vk::Offset3D{blit_texture->dst_end_x_, blit_texture->dst_end_y_, blit_texture->dst_end_z_}
    };
    auto region = vk::ImageBlit2()
        .setSrcSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(src_texture->GetImageAspect())
            .setMipLevel(blit_texture->src_mip_)
            .setBaseArrayLayer(blit_texture->src_base_layer_)
            .setLayerCount(blit_texture->src_layer_count_))
        .setDstSubresource(vk::ImageSubresourceLayers()
            .setAspectMask(dst_texture->GetImageAspect())
            .setMipLevel(blit_texture->dst_mip_)
            .setBaseArrayLayer(blit_texture->dst_base_layer_)
            .setLayerCount(blit_texture->dst_layer_count_))
        .setSrcOffsets(src_offsets)
        .setDstOffsets(dst_offsets);
    auto & blit_info = vk::BlitImageInfo2()
            .setSrcImage(src_texture->GetImage())
            .setDstImage(dst_texture->GetImage())
            .setSrcImageLayout(src_texture->GetImageLayout())
            .setDstImageLayout(dst_texture->GetImageLayout())
            .setRegions(region)
            .setFilter(GetVulkanFilter(blit_texture->filter_));
    CheckImageLayout(src_texture, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral);
    CheckImageLayout(dst_texture, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
    cmd.blitImage2(blit_info);
}


void VulkanCommandExecutor::RHIBeginRendering(RHICommandQueueBase *cmd, [[maybe_unused]] RHICommandBeginRendering *begin_rendering) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    [[maybe_unused]] auto & graphics = state.points[(uint32_t)RHIBindPointType::kGraphics];

    vk::Rect2D render_area = state.GetScissorRect();
    vk::RenderingAttachmentInfo attachments_info[C::kRHIMaxNumFramebufferAttachments];
    for(int i = 0; i < (int)state.draw_state_.num_framebuffer_attachments_; i++) {
        auto tex = (VulkanTexture*)state.draw_state_.attachments[i];
        if (tex) {
            assert(state.draw_state_.layers[i] < tex->GetArrayLayers());
            attachments_info[i] = vk::RenderingAttachmentInfo{
                tex->GetImageViewForLayer(state.draw_state_.layers[i]),
                tex->GetImageLayout(),
                {}, {}, {},
                GetVulkanLoadOp(state.draw_state_.load_ops[i]),
                GetVulkanStoreOp(state.draw_state_.store_ops[i]),
                {state.draw_state_.clear_values[i]}
            };
        } else {
            attachments_info[i] = vk::RenderingAttachmentInfo{};
        }
    }
    vk::RenderingAttachmentInfo depth_stencil_info {};
    if (auto ptr = state.draw_state_.depth_stencil_attachment) {
        auto vk_ptr = (VulkanTexture*)ptr;
        if (vk_ptr) {
            depth_stencil_info.imageView = vk_ptr->GetImageView();
            depth_stencil_info.imageLayout = vk_ptr->GetImageLayout();
            depth_stencil_info.loadOp = GetVulkanLoadOp(state.draw_state_.depth_stencil_load_op);
            depth_stencil_info.storeOp = GetVulkanStoreOp(state.draw_state_.depth_stencil_store_op);
            depth_stencil_info.clearValue = {state.draw_state_.depth_stencil_clear_value};
        }
    }
    // Sometimes, the render area can be 0-sized (e.g. rasterizationDiscard is enabled)
    // We clamp to 1x1 to avoid validation errors in such cases.
    render_area.extent.width = std::max(render_area.extent.width, 1u);
    render_area.extent.height = std::max(render_area.extent.height, 1u);
    auto rendering_info = vk::RenderingInfo {
            {}, render_area, 1, {}, state.draw_state_.num_framebuffer_attachments_, attachments_info,
            state.draw_state_.depth_stencil_attachment ? &depth_stencil_info : nullptr
    };
    state.cmd.beginRendering(rendering_info);
}

void VulkanCommandExecutor::RHIEndRendering(RHICommandQueueBase *cmd, [[maybe_unused]] RHICommandEndRendering *end_rendering) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    state.cmd.endRendering();
}

const static vk::ShaderStageFlags kBasicDrawStages =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl
| vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry
| vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eTaskEXT | vk::ShaderStageFlagBits::eMeshEXT;

void VulkanCommandExecutor::CheckDrawReadyness(RHICommandQueueBase *cmd) {

    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & graphics = state.points[(uint32_t)RHIBindPointType::kGraphics];
    // Check the compatibility of the bound pipeline and the bound framebuffer
    mi_assert(graphics.bound_pipeline, "No graphics pipeline bound");
    auto graphics_pipeline = (RHIGraphicsPipeline*)graphics.bound_pipeline;
    auto depth_enabled = graphics_pipeline->IsDepthTestEnabled();
    mi_assert(graphics_pipeline->GetFragmentOutputDesc().size()
              == state.draw_state_.num_framebuffer_attachments_,
              "Mismatched number of framebuffer attachments and fragment outputs");
    if(depth_enabled) {
        mi_assert(
                state.draw_state_.depth_stencil_attachment &&
                IsDepthStencilPixelFormat(state.draw_state_.depth_stencil_attachment->GetFormat()),
                  "Depth test enabled but no depth attachment found / invalid depth attachment pixel format");
    }
}


void VulkanCommandExecutor::RHIDraw(RHICommandQueueBase *cmd,
                                             RHICommandDraw *draw_primitive) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

    CheckDrawReadyness(cmd);

    state.InstallDrawState(state.cmd);
    FlushBindPointState(cmd, RHIBindPointType::kGraphics, kBasicDrawStages);
    state.cmd.draw(draw_primitive->vertex_count_, draw_primitive->instance_count_, draw_primitive->first_vertex_, draw_primitive->first_instance_);
}

void VulkanCommandExecutor::RHIDrawIndexed(RHICommandQueueBase *cmd,
                                                    RHICommandDrawIndexed *draw_indexed_primitive) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

    CheckDrawReadyness(cmd);

    state.InstallDrawState(state.cmd);
    FlushBindPointState(cmd, RHIBindPointType::kGraphics, kBasicDrawStages);

    state.BindIndexBuffer(draw_indexed_primitive->index_buffer_, draw_indexed_primitive->index_type_);
    state.cmd.drawIndexed(draw_indexed_primitive->index_count_,
                          draw_indexed_primitive->instance_count_,
                          draw_indexed_primitive->first_index_,
                          draw_indexed_primitive->base_vertex_index_,
                          draw_indexed_primitive->first_instance_index_);
}

void VulkanCommandExecutor::RHIDrawIndirect(RHICommandQueueBase *cmd, RHICommandDrawIndirect *draw_indirect) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

    CheckDrawReadyness(cmd);

    state.InstallDrawState(state.cmd);
    FlushBindPointState(cmd, RHIBindPointType::kGraphics, kBasicDrawStages);

    state.cmd.drawIndirect(
        static_cast<VulkanBuffer*>(draw_indirect->command_.buffer)->GetBuffer(),
        draw_indirect->command_.offset,
        draw_indirect->count_,
        sizeof(RHIDrawIndirectCommand)
    );
}

void VulkanCommandExecutor::RHIDrawIndexedIndirect(RHICommandQueueBase *cmd,
                                                    RHICommandDrawIndexedIndirect *draw_indexed_indirect) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

    CheckDrawReadyness(cmd);

    state.InstallDrawState(state.cmd);
    FlushBindPointState(cmd, RHIBindPointType::kGraphics, kBasicDrawStages);

    state.BindIndexBuffer(draw_indexed_indirect->index_buffer_, draw_indexed_indirect->index_type_);
    auto vk_buffer = static_cast<VulkanBuffer*>(draw_indexed_indirect->indirect_buffer_.buffer); // NOLINT its safe
    state.cmd.drawIndexedIndirect(
        vk_buffer->GetBuffer(),
        draw_indexed_indirect->indirect_buffer_.offset,
        draw_indexed_indirect->draw_count_, sizeof(RHIDrawIndexedIndirectCommand)
    );
}

void VulkanCommandExecutor::RHIDispatch(RHICommandQueueBase *cmd, RHICommandDispatch *dispatch) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    FlushBindPointState(cmd, RHIBindPointType::kCompute, vk::ShaderStageFlagBits::eCompute);
    state.cmd.dispatch(dispatch->group_count_x_, dispatch->group_count_y_, dispatch->group_count_z_);
}

void VulkanCommandExecutor::RHIDispatchIndirect(RHICommandQueueBase *cmd, RHICommandDispatchIndirect *dispatch_indirect) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    FlushBindPointState(cmd, RHIBindPointType::kCompute, vk::ShaderStageFlagBits::eCompute);
    auto vk_buffer = static_cast<VulkanBuffer*>(dispatch_indirect->dispatch_command_buffer_); // NOLINT its safe
    assert(vk_buffer && vk_buffer->GetBuffer() != nullptr);
    state.cmd.dispatchIndirect(vk_buffer->GetBuffer(), dispatch_indirect->offset_);
}

void VulkanCommandExecutor::RHIBindGraphicsPipeline(RHICommandQueueBase *cmd,
                                                    RHICommandBindGraphicsPipeline *bind_graphics_pipeline) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    assert(bind_graphics_pipeline->pipeline_->GetType() == RHIPipelineType::kGraphics);
    auto pipeline = static_cast<VulkanGraphicsPipeline*>(bind_graphics_pipeline->pipeline_); // NOLINT its safe
    auto & point = state.points[(uint32_t)RHIBindPointType::kGraphics];
    if(point.bound_pipeline != pipeline) {
        point.bound_pipeline_dirty = true;
        point.bound_private_descriptor_set = nullptr;
        point.bound_pipeline = pipeline;
    }
}

void VulkanCommandExecutor::RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetScissor *set_scissor) {
    CHECK_RHI_THREAD();
    state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current().draw_state_.scissor = {
        set_scissor->x_, set_scissor->y_, set_scissor->width_, set_scissor->height_
    };
}

void VulkanCommandExecutor::RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetCullMode *set_cull_mode) {
    CHECK_RHI_THREAD();
    state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current().draw_state_.cull_mode = set_cull_mode->cull_mode_;
}

void VulkanCommandExecutor::RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetViewport *set_viewport) {
    CHECK_RHI_THREAD();
    state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current().draw_state_.viewport = {
        set_viewport->x_, set_viewport->y_, set_viewport->width_, set_viewport->height_,
        set_viewport->min_depth_, set_viewport->max_depth_
    };
}

void
VulkanCommandExecutor::RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandUpdateDrawState *update_draw_state) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    state.draw_state_ = update_draw_state->draw_state_;
}

void VulkanCommandExecutor::RHIBindComputePipeline(
        RHICommandQueueBase *cmd, RHICommandBindComputePipeline *bind_compute_pipeline) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    assert(bind_compute_pipeline->pipeline_->GetType() == RHIPipelineType::kCompute);
    auto pipeline = static_cast<VulkanComputePipeline*>(bind_compute_pipeline->pipeline_); // NOLINT its safe
    auto & point = state.points[(uint32_t)RHIBindPointType::kCompute];
    if(point.bound_pipeline != pipeline) {
        point.bound_pipeline_dirty = true;
        point.bound_private_descriptor_set = nullptr;
        point.bound_pipeline = pipeline;
    }
}

void VulkanCommandExecutor::RHIBindPipelineParameters(
        RHICommandQueueBase *cmd, RHICommandBindPipelineParameters *bind_pipeline_parameters) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current(false);
    auto table = bind_pipeline_parameters->table_;
    auto & point = state.points[(uint32_t)bind_pipeline_parameters->point_];
    point.bound_descriptor_dirty |= point.parameter_table.Merge(&table);
}

void VulkanCommandExecutor::RHIBindVertexBuffer(RHICommandQueueBase *cmd,
                                               RHICommandBindVertexBuffer *bind_vertex_buffer) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & vb = bind_vertex_buffer->buffer_;
    auto * buffer = static_cast<VulkanBuffer*>(vb.buffer); // NOLINT its safe
    state.cmd.bindVertexBuffers(bind_vertex_buffer->binding_, buffer->GetBuffer(), vb.offset);
    mi_assert(std::size(state.bound_vertex_buffers) > bind_vertex_buffer->binding_, "Invalid binding");
    state.bound_vertex_buffers[bind_vertex_buffer->binding_] = vb;
}

void VulkanCommandExecutor::RHIClearBoundState(RHICommandQueueBase *buffer, RHICommandClearBoundState *cmd) {
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current(false);
    auto & point = state.points[(uint32_t)cmd->point_];
    // Simply clearing the parameter table is enough.
    point.parameter_table.Clear();
    // There's nothing more to do for different points.
}

void VulkanCommandExecutor::RHIFrameEnd(RHICommandQueueBase *cmd, RHISyncPoint * sync) {
    CHECK_RHI_THREAD();
    auto & chain = state_chains_[(uint32_t)cmd->GetCommandQueueType()];
    auto & state = chain.Current();

    state.CheckDebugMarkerStack();
    std::string prefix;
#ifndef NDEBUG
    prefix = "EndOfFrame (" + std::to_string(GetFrameIndexForCurrentThread()) + ")";
#endif

    auto vk_rhi = GetVulkanRHI();
    if (!vk_rhi->IsSwapChainInitialized()) {
        // Offscreen rendering, no need for presenting, simply do a submission
        RHISubmitCommandBuffer(cmd, sync, prefix, false);
    } else {
        // Do present if needed
        auto queue = vk_rhi->GetQueue(cmd->GetCommandQueueType());
        auto device = vk_rhi->GetDevice();
        // 1. acquire the next swapchain image
        auto swapchain = vk_rhi->GetSwapChain();
        uint32_t swapchain_image_index;
        vk::Semaphore image_ready_sem = vk_rhi->vk_swapchain_image_available_semaphores_[
            GetCurrentFrameIndex_RHIThread() % vk_rhi->swapchain_images.size()
        ];
        auto result = device.acquireNextImageKHR(swapchain, C::kMaxFrameTimeoutNanoseconds, image_ready_sem, {}, &swapchain_image_index);
        if (result == vk::Result::eErrorOutOfDateKHR) {
            MI_LOG(MIInfraLogType::kError, "Swapchain out of date");
            return;
        } else if (result != vk::Result::eSuccess) {
            MI_LOG(MIInfraLogType::kError, "Failed to acquire swapchain image: {}", vk::to_string(result));
            return;
        }
        // 2. copy backbuffer to swapchain image
        state.BeginCmd();
        // 2.1 transit image layout
        auto backbuffer = vk_rhi->GetBackBufferForFrameIndex(GetCurrentFrameIndex_RHIThread());
        vk::Image swapchain_image = vk_rhi->swapchain_images[swapchain_image_index];
        vk::ImageMemoryBarrier image_barrier_x {
            vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            vk::AccessFlagBits::eTransferRead,
            GetVulkanImageLayout(backbuffer->GetLayout_RHIThread()),
            vk::ImageLayout::eTransferSrcOptimal,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            ((VulkanTexture*)backbuffer)->GetImage(),
            vk::ImageSubresourceRange {
                vk::ImageAspectFlagBits::eColor,
                0, 1, 0, 1
            }
        };
        vk::ImageMemoryBarrier image_barrier_y = image_barrier_x;
        image_barrier_y.image = swapchain_image;
        image_barrier_y.oldLayout = vk::ImageLayout::eUndefined;
        image_barrier_y.newLayout = vk::ImageLayout::eTransferDstOptimal;
        image_barrier_y.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        {
            vk::ImageMemoryBarrier barriers[] = {image_barrier_x, image_barrier_y};
            state.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer, {},
                {}, {}, barriers);
            ((VulkanTexture*)backbuffer)->vk_image_layout_ = vk::ImageLayout::eTransferSrcOptimal;
            ((VulkanTexture*)backbuffer)->layout_ = RHITextureLayoutType::kTransferSrcOptimal;
            ((VulkanTexture*)backbuffer)->using_stages = vk::PipelineStageFlagBits::eTransfer;
            // The images in the swapchain are not used by the application, so we don't need to update the layout
        }
        // 2.2 copy backbuffer to swapchain image
        auto copy_region = vk::ImageCopy {
            vk::ImageSubresourceLayers {
                vk::ImageAspectFlagBits::eColor,
                0, 0, 1
            },
            {0, 0, 0},
            vk::ImageSubresourceLayers {
                vk::ImageAspectFlagBits::eColor,
                0, 0, 1
            },
            {0, 0, 0},
            {backbuffer->GetWidth(), backbuffer->GetHeight(), backbuffer->GetDepth()}
        };
        state.cmd.copyImage(
            ((VulkanTexture*)backbuffer)->GetImage(),
            vk::ImageLayout::eTransferSrcOptimal,
            swapchain_image,
            vk::ImageLayout::eTransferDstOptimal,
            copy_region
        );
        // 2.3 transit swapchain image layout
        vk::ImageMemoryBarrier swapchain_barrier {
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eMemoryRead,
            vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::ePresentSrcKHR,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            swapchain_image,
            vk::ImageSubresourceRange {
                vk::ImageAspectFlagBits::eColor,
                0, 1, 0, 1
            }
        };
        state.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
            {}, {}, {}, swapchain_barrier);
        // 2.4 Execution barrier, make sure all previously submitted commands are finished before this one finishes
        // Thus the completion of this command buffer will mark the end of the whole frame.
        state.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eNone,
            {}, {}, {}, {});
        // 25.8.14: Vulkan validation layer synchronization false positive. Adding a mega barrier for now.
        // 25.10.31: this seems to be a bug relating to my ClearBuffer command. Removing the mega barrier.
        // state.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllGraphics, vk::PipelineStageFlagBits::eAllGraphics,
        //     {}, vk::MemoryBarrier{
        //         vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        //         vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite
        //     }, {}, {});
        // 3. end and submit command buffer.
        state.CloseCmd();
        // Use the render-finished semaphore associated with the acquired swapchain image index
        vk::Semaphore present_ready_sem = vk_rhi->vk_swapchain_render_finished_semaphores_[
            swapchain_image_index
        ];
        vk::PipelineStageFlags submit_wait_stages = vk::PipelineStageFlagBits::eTransfer;
        if (!prefix.empty()) {
#ifndef NDEBUG
            GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
                vk::DebugUtilsObjectNameInfoEXT {
                vk::ObjectType::eCommandBuffer, reinterpret_cast<uint64_t>((VkCommandBuffer)state.cmd),
                prefix.c_str()
            });
#endif
        }
        auto submit_info = vk::SubmitInfo {
            1, &image_ready_sem, &submit_wait_stages,
            1, &state.cmd,
            1, &present_ready_sem
        };
        auto vk_sync = (VulkanSyncPoint*)sync;
        queue.submit(submit_info, vk_sync ? vk_sync->GetFence() : nullptr);
        // Submission is done, we can reset the command buffer handle to make sure it is not used again
        state.cmd = nullptr;
        // 4. present the swapchain image
        vk::Result present_result;
        auto present_info = vk::PresentInfoKHR {
            1, &present_ready_sem, 1, &swapchain,
            &swapchain_image_index, &present_result
        };
        result = queue.presentKHR(present_info);
        if (result != vk::Result::eSuccess) {
            MI_LOG(MIInfraLogType::kError, "Failed to present swapchain image: {}", vk::to_string(result));
        }
    }

    // Switch double buffered states
    {
        // Release resources allocated for the frame before current frame
        int prev_state_index = (chain.state_index - 1 + (int) std::size(chain.states)) % (int) std::size(chain.states);
        auto &prev_state = chain.states[prev_state_index];
        prev_state.Clear(false);
        // Switch to the next state
        chain.state_index = (chain.state_index + 1) % (int) std::size(chain.states);
    }
    if (sync) ((VulkanSyncPoint*)sync)->NotifySubmission();
}

// Helpers

vk::Rect2D VulkanCommandExecutor::CommandQueueState::GetScissorRect() {
    CHECK_RHI_THREAD();
    vk::Rect2D rect = {
        {draw_state_.scissor.x, draw_state_.scissor.y},
        {draw_state_.scissor.width, draw_state_.scissor.height}
    };
    if(rect.extent.width == 0 && rect.extent.height == 0
       && rect.offset.x == 0 && rect.offset.y == 0) {
        // Default to the size of the bound framebuffer if not set
        if(draw_state_.attachments[0]) {
            rect.extent.width = draw_state_.attachments[0]->GetWidth();
            rect.extent.height = draw_state_.attachments[0]->GetHeight();
        } else {
            auto pipeline = points[(uint32_t)RHIBindPointType::kGraphics].bound_pipeline;
            if (pipeline) {
                auto graphics_pipeline = (RHIGraphicsPipeline*)pipeline;
                if (graphics_pipeline->GetFragmentOutputDesc().size() == 0) {
                    // No outputs, scissor does not matter
                    rect = vk::Rect2D{
                        vk::Offset2D{0,0},
                        vk::Extent2D{0u,0u}
                };
                } else {
                    MI_LOG(MIInfraLogType::kWarning, "Scissor not set and no framebuffer bound,"
                                                     "while the bound graphics pipeline has outputs.");
                }
            }
        }
    }
    return rect;
}

vk::Viewport VulkanCommandExecutor::CommandQueueState::GetViewport() {
    CHECK_RHI_THREAD();
    vk::Viewport viewport = {
        draw_state_.viewport.x, draw_state_.viewport.y,
        draw_state_.viewport.width, draw_state_.viewport.height,
        draw_state_.viewport.min_depth, draw_state_.viewport.max_depth
    };
    if(viewport.width == 0 && viewport.height == 0
       && viewport.x == 0 && viewport.y == 0) {
        // Default to the size of the bound framebuffer if not set
        if(draw_state_.attachments[0]) {
            viewport.width = (float)draw_state_.attachments[0]->GetWidth();
            viewport.height = (float)draw_state_.attachments[0]->GetHeight();
        } else {
            auto pipeline = points[(uint32_t)RHIBindPointType::kGraphics].bound_pipeline;
            if (pipeline) {
                auto graphics_pipeline = (RHIGraphicsPipeline*)pipeline;
                if (graphics_pipeline->GetFragmentOutputDesc().size() == 0) {
                    // No outputs, viewport does not matter
                    viewport = vk::Viewport{
                        0.0f, 0.0f,
                        0.0f, 0.0f,
                        0.0f, 1.0f
                    };
                } else {
                    MI_LOG(MIInfraLogType::kWarning, "Viewport not set and no framebuffer bound,"
                                                     "while the bound graphics pipeline has outputs.");
                }
            }
        }
    }
    // Invert y-axis to match D3D12 and OpenGL conventions of the NDC
    // Note: In Vulkan, NDC y is positive downwards (rhs consistency), but in D3D12 and OpenGL, it is positive upwards (lhs).
    vk::Viewport vk_viewport = viewport;
    vk_viewport.y += viewport.height;
    vk_viewport.height = -viewport.height;
    return vk_viewport;
}


void VulkanCommandExecutor::CommandQueueState::InstallDrawState(vk::CommandBuffer cmdb) {
    // TODO lazy install
    auto rect = GetScissorRect();
    auto viewport = GetViewport();
    // Bypass validation errors. (no viewport usually means no rasterization, so it does not matter)
    if (viewport.width == 0) viewport.width = 1.0f;
    if (viewport.height == 0) viewport.height = 1.0f;
    cmdb.setViewportWithCount(viewport);
    cmdb.setScissorWithCount(rect);
    cmdb.setCullMode(GetVulkanCullMode(draw_state_.cull_mode));
    cmdb.setPolygonModeEXT(GetVulkanPolygonMode(draw_state_.polygon_mode));
    cmdb.setLineWidth(draw_state_.line_width);
}

bool VulkanCommandExecutor::CommandQueueState::BindPoint::ParameterTable::Merge (const RHIBindPipelineParametersDesc * desc) {
    CHECK_RHI_THREAD();
    auto CompareAndInsert = [&] <typename T>  (std::vector<T> & dst, std::span<T> src) {
        bool dirty = false;
        for (const auto & e : src) {
            auto it = std::find_if(dst.begin(), dst.end(), [&](const auto & a) {
                return a.slot == e.slot;
            });
            if (it == dst.end()) {
                dst.push_back(e);
                dirty = true;
            } else {
                if constexpr (std::is_same_v<T, RHIPipelineParameterBufferDesc>) {
                    if (it->buffer.buffer != e.buffer.buffer || it->buffer.offset != e.buffer.offset || it->buffer.size != e.buffer.size) {
                        *it = e;
                        dirty = true;
                    }
                }
                if constexpr (std::is_same_v<T, RHIPipelineParameterTextureDesc>) {
                    if (it->texture != e.texture || it->array_layer != e.array_layer) {
                        *it = e;
                        dirty = true;
                    }
                }
                if constexpr (std::is_same_v<T, RHIPipelineParameterResourceDesc>) {
                    if (it->resource != e.resource) {
                        *it = e;
                        dirty = true;
                    }
                }
            }
        }
        return dirty;
    };
    bool dirty = false;
    dirty |= CompareAndInsert(uniforms, desc->uniforms);
    dirty |= CompareAndInsert(storages, desc->storages);
    dirty |= CompareAndInsert(uavs, desc->uavs);
    dirty |= CompareAndInsert(srvs, desc->srvs);
    dirty |= CompareAndInsert(samplers, desc->samplers);
    dirty |= CompareAndInsert(acceleration_structures, desc->acceleration_structures);
    // Overwrite push constants if any (and it does not affect the dirty flag)
    if(!desc->constants.empty()) {
        push_constants = desc->constants;
    }
    return dirty;
}

// TODO remove the [[maybe_unused]] stuff.
VulkanCommandExecutor::DescriptorWrites
VulkanCommandExecutor::CommandQueueState::BindPoint::CompileShaderDescriptorWrites(
    CommandQueueState & state, [[maybe_unused]] vk::Device device,
    vk::DescriptorSet descriptor_set,
    [[maybe_unused]] vk::CommandBuffer cmdb
) {
    CHECK_RHI_THREAD();

    // Sort and merge all recorded slot bindings
    auto SortUnique = [&](auto & arr) {
        std::stable_sort(arr.begin(), arr.end(), [](const auto & a, const auto & b) {
            return a.slot < b.slot;
        });
        std::reverse(arr.begin(), arr.end());
        auto tail = std::unique(arr.begin(), arr.end(), [](const auto & a, const auto & b) {
            return a.slot == b.slot;
        });
        arr.erase(tail, arr.end());
    };
    SortUnique(parameter_table.uniforms);
    SortUnique(parameter_table.storages);
    SortUnique(parameter_table.uavs);
    SortUnique(parameter_table.srvs);
    SortUnique(parameter_table.samplers);
    SortUnique(parameter_table.acceleration_structures);

    const VulkanPipelineBindingRemappings * remapping = nullptr;
    assert(bound_pipeline);
    if (bind_point_type == RHIBindPointType::kGraphics) {
        remapping = &((VulkanGraphicsPipeline*)bound_pipeline)->GetRemappings();
    } else if (bind_point_type == RHIBindPointType::kCompute) {
        remapping = &((VulkanComputePipeline*)bound_pipeline)->GetRemappings();
    } else if (bind_point_type == RHIBindPointType::kRayTracing) {
        remapping = &((VulkanRayTracingPipeline*)bound_pipeline)->GetRemappings();
    } else {
        assert(false && "Not implemented");
    }

    // Count all writes that needed to allocate a WriteDescriptorSet array
    auto write_count = (uint32_t)
            (parameter_table.uniforms.size() + parameter_table.storages.size() + parameter_table.uavs.size()
                + parameter_table.srvs.size() + parameter_table.samplers.size() + parameter_table.acceleration_structures.size());
    vk::WriteDescriptorSet * writes = state.Allocate<vk::WriteDescriptorSet[]>(write_count);
    size_t write_index = 0;
    for(auto ubo : parameter_table.uniforms) {
        auto& buffer_info = *state.Allocate<vk::DescriptorBufferInfo>();
        assert(ubo.buffer.buffer && "Uniform buffer must not be null.");
        buffer_info.buffer = static_cast<VulkanBuffer*>(ubo.buffer.buffer)->GetBuffer(); // NOLINT its safe
        buffer_info.offset = ubo.buffer.offset;
        buffer_info.range = ubo.buffer.size;
        auto destination = remapping->GetDestination(RHIPipelineResourceType::kUniformBuffer, ubo.slot);
        if (UINT32_MAX != destination.binding) {
            auto write = vk::WriteDescriptorSet()
                .setDstSet(descriptor_set)
                .setDstBinding(destination.binding)
                .setDstArrayElement(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setPBufferInfo(&buffer_info);
            writes[write_index++] = write;
        } else {
            MI_WARN("Uniform buffer slot {} is not present in bound pipeline {}.", ubo.slot, bound_pipeline->GetName());
        }
    }
    for(auto storage : parameter_table.storages) {
        auto& buffer_info = *state.Allocate<vk::DescriptorBufferInfo>();
        auto buffer = static_cast<VulkanBuffer*>(storage.buffer.buffer); // NOLINT its safe
        auto buffer_ptr = buffer ? buffer->GetBuffer() : nullptr;
        buffer_info.buffer = buffer_ptr;
        buffer_info.offset = storage.buffer.offset;
        buffer_info.range = buffer_ptr ? storage.buffer.size : VK_WHOLE_SIZE;
        auto destination = remapping->GetDestination(RHIPipelineResourceType::kStorageBuffer, storage.slot);
        if (UINT32_MAX != destination.binding) {
            auto write = vk::WriteDescriptorSet()
                    .setDstSet(descriptor_set)
                    .setDstBinding(remapping->GetDestination(RHIPipelineResourceType::kStorageBuffer, storage.slot).binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eStorageBuffer)
                    .setPBufferInfo(&buffer_info);
            writes[write_index++] = write;
        } else {
            MI_WARN("Storage buffer slot {} is not present in bound pipeline {}.", storage.slot, bound_pipeline->GetName());
        }
    }
    for(auto uav : parameter_table.uavs) {
        auto& image_info = *state.Allocate<vk::DescriptorImageInfo>();
        auto image = static_cast<VulkanTexture*>(uav.texture);
        if (uav.array_layer == UINT_MAX && uav.mip_level == 0)
            image_info.imageView = image ? image->GetImageView() : nullptr;
        else image_info.imageView = image ? image->GetImageViewForLayer(
            uav.array_layer == UINT_MAX ? 0 : uav.array_layer, uav.mip_level
        ) : nullptr;
        image_info.imageLayout = vk::ImageLayout::eGeneral;
        auto destination = remapping->GetDestination(RHIPipelineResourceType::kUAV, uav.slot);
        if (UINT_MAX != destination.binding) {
            auto write = vk::WriteDescriptorSet()
                    .setDstSet(descriptor_set)
                    .setDstBinding(remapping->GetDestination(RHIPipelineResourceType::kUAV, uav.slot).binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eStorageImage)
                    .setPImageInfo(&image_info);
            writes[write_index++] = write;
        } else {
            MI_WARN("UAV slot {} is not present in bound pipeline {}.", uav.slot, bound_pipeline->GetName());
        }
    }
    for(auto srv : parameter_table.srvs) {
        auto& image_info = *state.Allocate<vk::DescriptorImageInfo>();
        auto image = static_cast<VulkanTexture*>(srv.texture);
        if (srv.array_layer == UINT_MAX)
            image_info.imageView = image ? image->GetImageView() : nullptr;
        else image_info.imageView = image ? image->GetImageViewForLayer(srv.array_layer, srv.mip_level) : nullptr;
        image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        auto destination = remapping->GetDestination(RHIPipelineResourceType::kSRV, srv.slot);
        if (UINT32_MAX != destination.binding) {
            auto write = vk::WriteDescriptorSet()
                    .setDstSet(descriptor_set)
                    .setDstBinding(remapping->GetDestination(RHIPipelineResourceType::kSRV, srv.slot).binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eSampledImage) // TODO This may not work with Texture.Load
                    .setPImageInfo(&image_info);
            writes[write_index++] = write;
        } else {
            MI_WARN("SRV slot {} is not present in bound pipeline {}.", srv.slot, bound_pipeline->GetName());
        }
    }
    for(auto sampler : parameter_table.samplers) {
        auto& image_info = *state.Allocate<vk::DescriptorImageInfo>();
        assert(sampler.resource && "Sampler must not be null.");
        image_info.sampler = static_cast<VulkanSampler*>(sampler.resource)->GetSampler(); // NOLINT its safe
        auto destination = remapping->GetDestination(RHIPipelineResourceType::kSampler, sampler.slot);
        if (UINT32_MAX != destination.binding) {
            auto write = vk::WriteDescriptorSet()
                    .setDstSet(descriptor_set)
                    .setDstBinding(remapping->GetDestination(RHIPipelineResourceType::kSampler, sampler.slot).binding)
                    .setDstArrayElement(0)
                    .setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eSampler)
                    .setPImageInfo(&image_info);
            writes[write_index++] = write;
        } else {
            MI_WARN("Sampler slot {} is not present in bound pipeline {}.", sampler.slot, bound_pipeline->GetName());
        }
    }
    for(auto acc : parameter_table.acceleration_structures) {
        auto& write_khr = *state.Allocate<vk::WriteDescriptorSetAccelerationStructureKHR>();
        auto  p_ac = state.Allocate<vk::AccelerationStructureKHR>();
        auto destination = remapping->GetDestination(RHIPipelineResourceType::kAccelerationStructure, acc.slot);
        if (UINT32_MAX != destination.binding) {
            auto write = vk::WriteDescriptorSet()
                    .setDstSet(descriptor_set)
                    .setDstBinding(remapping->GetDestination(RHIPipelineResourceType::kAccelerationStructure, acc.slot).binding)
                    .setDescriptorCount(1)
                    .setDescriptorType(vk::DescriptorType::eAccelerationStructureKHR)
                    .setPNext(&write_khr);
            write_khr.accelerationStructureCount = 1;
            auto rhi_acc = static_cast<VulkanAccelerationStructure*>(acc.resource);
            *p_ac = rhi_acc ? rhi_acc->GetAccelerationStructure() : nullptr;
            write_khr.pAccelerationStructures = p_ac;
            writes[write_index++] = write;
        } else {
            MI_WARN("Acceleration structure slot {} is not present in bound pipeline {}.", acc.slot, bound_pipeline->GetName());
        }
    }

    return {writes, write_index};
}

void VulkanCommandExecutor::CommandQueueState::BindPoint::ParameterTable::Clear() {
    // Clear all bindings
    uniforms.clear();
    storages.clear();
    uavs.clear();
    srvs.clear();
    samplers.clear();
    acceleration_structures.clear();
}

// Bind pipeline, descriptor set and flush descriptor writes.
void VulkanCommandExecutor::FlushBindPointState(
        RHICommandQueueBase * cmd, RHIBindPointType point_t, vk::ShaderStageFlags use_shaders) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto & point = state.points[(uint32_t)point_t];

    vk::PipelineBindPoint vk_point {};
    vk::PipelineLayout vk_pipeline_layout {};
    vk::Pipeline vk_pipeline {};
    vk::DescriptorSetLayout vk_set_layout {};

    if (point_t == RHIBindPointType::kGraphics) {
        auto g_pipeline = (VulkanGraphicsPipeline*)point.bound_pipeline;
        vk_pipeline_layout = g_pipeline->GetPipelineLayout();
        vk_set_layout = g_pipeline->GetPrivateDescriptorSetLayout();
        vk_pipeline = g_pipeline->GetPipeline();
        vk_point = vk::PipelineBindPoint::eGraphics;
    } else if (point_t == RHIBindPointType::kCompute) {
        auto c_pipeline = (VulkanComputePipeline*)point.bound_pipeline;
        vk_pipeline_layout = c_pipeline->GetPipelineLayout();
        vk_set_layout = c_pipeline->GetPrivateDescriptorSetLayout();
        vk_pipeline = c_pipeline->GetPipeline();
        vk_point = vk::PipelineBindPoint::eCompute;
    } else {
        auto rt_pipeline = (VulkanRayTracingPipeline*)point.bound_pipeline;
        vk_point = vk::PipelineBindPoint::eRayTracingKHR;
        vk_pipeline_layout = rt_pipeline->GetPipelineLayout();
        vk_set_layout = rt_pipeline->GetPrivateDescriptorSetLayout();
        vk_pipeline = rt_pipeline->GetPipeline();
        vk_point = vk::PipelineBindPoint::eRayTracingKHR;
    }

    // Rebind pipeline if dirty
    if (point.bound_pipeline_dirty) {
        point.bound_descriptor_dirty = true;
        state.cmd.bindPipeline(vk_point, vk_pipeline);
        // Bind the bindless descriptor set upon pipeline binding (at binding 1)
        if (point.bound_pipeline->HasBindlessResources()) {
            auto bindless_set = GetVulkanRHI()->GetVulkanBindlessManager()->GetBindlessDescriptorSet();
            state.cmd.bindDescriptorSets(vk_point, vk_pipeline_layout, 1,
                                         bindless_set, {});
        }
    }

    // Allocate descriptor set for new pipeline or modified descriptors
    if(point.bound_descriptor_dirty && vk_set_layout) {
        auto descriptor_set = GetVulkanRHI()->GetDevice().allocateDescriptorSets(
                vk::DescriptorSetAllocateInfo()
                        .setDescriptorPool(state.descriptor_pool)
                        .setDescriptorSetCount(1)
                        .setSetLayouts(vk_set_layout)
        );
        mi_assert(!descriptor_set.empty(), "Failed to allocate descriptor set");
        point.bound_private_descriptor_set = descriptor_set[0];
    }

    auto descriptor_writes = point.CompileShaderDescriptorWrites(
            state, GetVulkanRHI()->GetDevice(), point.bound_private_descriptor_set,
            state.cmd
    );
    // Only write the descriptor set if it's dirty
    if(!descriptor_writes.empty() && point.bound_descriptor_dirty) {
        if(!point.bound_private_descriptor_set) {
            if(!point.bound_pipeline) {
                MI_LOG(MIInfraLogType::kWarning, "Flushed resources to null pipeline.");
            } else {
                MI_LOG(MIInfraLogType::kWarning, "Resources bound to fully bindless pipeline.");
            }
        }
        GetVulkanRHI()->GetDevice().updateDescriptorSets(descriptor_writes, {});
    }

    // Bind descriptor set
    // Non-bindless descriptor sets doesn't support update-after-bind. So we bind them at last.
    if(point.bound_descriptor_dirty && point.bound_private_descriptor_set) {
        state.cmd.bindDescriptorSets(vk_point, vk_pipeline_layout, 0,
                                     {point.bound_private_descriptor_set}, {});
    }
    point.bound_pipeline_dirty = false;
    point.bound_descriptor_dirty = false;

    // Push constants
    if(!point.parameter_table.push_constants.empty()) {
        state.cmd.pushConstants(vk_pipeline_layout, use_shaders,
            0, (uint32_t)point.parameter_table.push_constants.size() * sizeof(uint32_t),
            point.parameter_table.push_constants.data());
        point.parameter_table.push_constants = {};
    }
}

void VulkanCommandExecutor::RHITextureBarrier(RHICommandQueueBase *cmd,
                                              RHICommandTextureBarrier *barrier) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto barriers = state.Allocate<vk::ImageMemoryBarrier2[]>(barrier->num_textures_);
    for (int i = 0; i < (int)barrier->num_textures_; i++) {
        auto texture = static_cast<VulkanTexture*>(barrier->textures_[i]);
        auto dst_vk_layout = GetVulkanImageLayout(barrier->layouts_[i]);
        barriers[i] = vk::ImageMemoryBarrier2()
                .setSrcStageMask(GetVulkanPipelineStageFlags(barrier->src_stages_[i]))
                .setDstStageMask(GetVulkanPipelineStageFlags(barrier->dst_stages_[i]))
                .setSrcAccessMask(GetVulkanAccessFlags(barrier->src_accesses_[i]))
                .setDstAccessMask(GetVulkanAccessFlags(barrier->dst_accesses_[i]))
                .setOldLayout(texture->vk_image_layout_)
                .setNewLayout(dst_vk_layout)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setImage(texture->GetImage())
                .setSubresourceRange(vk::ImageSubresourceRange()
                        .setAspectMask(texture->GetImageAspect())
                        .setBaseMipLevel(0)
                        .setLevelCount(texture->GetMipLevels())
                        .setBaseArrayLayer(0)
                        .setLayerCount(texture->GetArrayLayers())
                );
        texture->layout_ = barrier->layouts_[i];
        texture->vk_image_layout_ = dst_vk_layout;
    }
    state.cmd.pipelineBarrier2(vk::DependencyInfo{
        {}, 0, {}, 0, {},
        barrier->num_textures_, barriers
    });
}

void
VulkanCommandExecutor::RHIMemoryBarrier(RHICommandQueueBase *cmd, RHICommandMemoryBarrier *barrier) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();
    auto memory_barrier = vk::MemoryBarrier2 {
        GetVulkanPipelineStageFlags(barrier->src_stages_),
        GetVulkanAccessFlags(barrier->src_accesses_),
        GetVulkanPipelineStageFlags(barrier->dst_stages_),
        GetVulkanAccessFlags(barrier->dst_accesses_)
    };
    state.cmd.pipelineBarrier2(
        vk::DependencyInfo {{},
            memory_barrier, {}, {}
        }
    );
}

void
VulkanCommandExecutor::RHIBufferBarriers(RHICommandQueueBase *cmd, RHICommandBufferBarrier *barrier) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)cmd->GetCommandQueueType()].Current();

    auto buffers = barrier->buffers_;
    auto vk_barriers = state.Allocate<vk::BufferMemoryBarrier2[]>(barrier->num_buffers_);
    for (const auto& [i, e] : std::views::enumerate(std::span(buffers, barrier->num_buffers_))) {
        vk_barriers[i].srcStageMask = GetVulkanPipelineStageFlags(barrier->src_stages_[i]);
        vk_barriers[i].dstStageMask = GetVulkanPipelineStageFlags(barrier->dst_stages_[i]);
        vk_barriers[i].srcAccessMask = (barrier->src_stages_[i] == RHIPipelineStageFlagBits::kNone)
            ? vk::AccessFlags2{} : GetVulkanAccessFlags(barrier->src_accesses_[i]);
        vk_barriers[i].dstAccessMask = (barrier->dst_stages_[i] == RHIPipelineStageFlagBits::kNone)
            ? vk::AccessFlags2{} : GetVulkanAccessFlags(barrier->dst_accesses_[i]);
        vk_barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barriers[i].buffer = ((VulkanBuffer*)buffers[i].buffer)->GetBuffer();
        vk_barriers[i].offset = buffers[i].offset;
        vk_barriers[i].size = buffers[i].size;
    }
    state.cmd.pipelineBarrier2(vk::DependencyInfo{
        {}, 0, nullptr, barrier->num_buffers_,
        vk_barriers, 0, nullptr
    });
}

void VulkanCommandExecutor::RHIDebugMarkerBegin(RHICommandQueueBase *buffer, RHICommandDebugMarkerBegin *cmd) {
    // Insert debug marker
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current();
    state.BeginCmd();
    state.cmd.beginDebugUtilsLabelEXT(
        vk::DebugUtilsLabelEXT()
            .setPLabelName(cmd->marker_name_)
            .setColor(cmd->color_)
    );
#ifndef NDEBUG
    state.debug_marker_stack.push(cmd->marker_name_);
#endif
}

void VulkanCommandExecutor::RHIDebugMarkerEnd(RHICommandQueueBase *buffer, [[maybe_unused]] RHICommandDebugMarkerEnd *cmd) {
    // Insert debug marker
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current();
    state.BeginCmd();
    state.cmd.endDebugUtilsLabelEXT();
    mi_assert(!state.debug_marker_stack.empty(), "Potential mismatch between begin and end debug markers");
#ifndef NDEBUG
    state.debug_marker_stack.pop();
#endif
}

void VulkanCommandExecutor::RHIDebugMarkerInsert(RHICommandQueueBase *buffer, RHICommandDebugMarkerInsert *cmd) {
    // Insert debug marker
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current();
    state.BeginCmd();
    state.cmd.insertDebugUtilsLabelEXT(
        vk::DebugUtilsLabelEXT()
            .setPLabelName(cmd->marker_name_)
            .setColor(cmd->color_)
    );
#ifndef NDEBUG
    state.last_inserted_debug_marker = cmd->marker_name_;
#endif
}

void VulkanCommandExecutor::RHIInsertTimestamp(RHICommandQueueBase * buffer, RHICommandInsertTimestamp *cmd) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current();
    auto vk_rhi = GetVulkanRHI();
    auto vk_timestamp = (VulkanTimestamp*)cmd->timestamp_;
    auto query = vk_timestamp->GetQueryIndex();
    state.BeginCmd();
    mi_assert(cmd->stage_ == RHIPipelineStageFlagBits::kAll, "Not implemented");
    auto pool = vk_rhi->GetTimestampQueryPool();
    state.cmd.resetQueryPool(pool, query, 1);
    state.cmd.writeTimestamp(
        vk::PipelineStageFlagBits::eAllCommands,
        pool, query
    );
}

void
VulkanCommandExecutor::RHISubmitCommandBuffer(RHICommandQueueBase *buffer, RHISyncPoint * sync,
const std::string & submit_prefix,
// TODO make this useful (or completely remove it)
[[maybe_unused]] bool recycle_resources) {
    CHECK_RHI_THREAD();
    auto & state = state_chains_[(uint32_t)buffer->GetCommandQueueType()].Current(false);
    if (sync) {
        // Place an execution barrier if sync point is specified, barrier the previously submitted commands
        state.BeginCmd();
        state.cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eNone,
            {}, {}, {}, {}
        );
    }
    auto & cmd = state.cmd;
    if (!submit_prefix.empty()) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
            vk::DebugUtilsObjectNameInfoEXT {
            vk::ObjectType::eCommandBuffer, reinterpret_cast<uint64_t>((VkCommandBuffer)cmd),
            submit_prefix.c_str()
        });
    }
    bool dirty = state.CloseCmd();
    auto vk_rhi = GetVulkanRHI();
    auto queue = vk_rhi->GetQueue(buffer->GetCommandQueueType());
    auto submit_info = vk::SubmitInfo()
            .setCommandBufferCount(1)
            .setPCommandBuffers(&cmd);
    if (dirty) queue.submit(submit_info, sync ? ((VulkanSyncPoint*)sync)->GetFence() : nullptr);

    // Reset the handle to the command buffer after submission
    state.cmd = nullptr;
    // Reset states
    state.ResetStates();

    if(sync) ((VulkanSyncPoint*)sync)->NotifySubmission();
}

MI_NAMESPACE_END
