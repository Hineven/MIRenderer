/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_cmd.h"
#include "rhi/rhi.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi_texture.h"
MI_NAMESPACE_BEGIN

void RHICommandBase::ExecuteAndDestruct(RHICommandQueueBase &cmd) {

}

void RHICommandClearTexture::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIClearTexture(&cmd, this);
}

void RHICommandCopyBufferToTexture::Execute(RHICommandQueueBase &cmd) {
    if(dst_tex_width_ == 0) {
        dst_tex_width_ = texture_->GetWidth();
    }
    if(dst_tex_height_ == 0) {
        dst_tex_height_ = texture_->GetHeight();
    }
    if(dst_tex_depth_ == 0) {
        dst_tex_depth_ = texture_->GetDepth();
    }
    RHI::Get().GetCommandExecutor()->RHICopyBufferToTexture(&cmd, this);
}

void RHICommandCopyTextureToBuffer::Execute(RHICommandQueueBase &cmd) {
    if(src_tex_width_ == 0) {
        src_tex_width_ = texture_->GetWidth();
    }
    if(src_tex_height_ == 0) {
        src_tex_height_ = texture_->GetHeight();
    }
    if(src_tex_depth_ == 0) {
        src_tex_depth_ = texture_->GetDepth();
    }
    RHI::Get().GetCommandExecutor()->RHICopyTextureToBuffer(&cmd, this);
}

void RHICommandCopyBuffer::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHICopyBuffer(&cmd, this);
}

void RHICommandCopyTexture::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHICopyTexture(&cmd, this);
}

void RHICommandBeginRendering::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIBeginRendering(&cmd, this);
}

void RHICommandEndRendering::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIEndRendering(&cmd, this);
}

void RHICommandUpdateDrawState::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}

void RHICommandDrawPrimitive::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIDrawPrimitive(&cmd, this);
}

void RHICommandDrawIndexedPrimitive::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIDrawIndexedPrimitive(&cmd, this);
}

void RHICommandSetScissor::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}

void RHICommandSetViewport::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}

void RHICommandDispatch::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIDispatch(&cmd, this);
}

void RHICommandBindGraphicsPipeline::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIBindGraphicsPipeline(&cmd, this);
}

void RHICommandBindComputePipeline::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIBindComputePipeline(&cmd, this);
}

void RHICommandBindPipelineParameters::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIBindPipelineParameters(&cmd, this);
}

void RHICommandBindVertexBuffer::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIBindVertexBuffer(&cmd, this);
}

void RHICommandTextureBarrier::Execute(mi::RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHITextureBarrier(&cmd, this);
}

void RHICommandBufferBarrier::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIBufferBarriers(&cmd, this);
}

MI_NAMESPACE_END





