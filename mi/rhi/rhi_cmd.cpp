/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_cmd.h"
#include "rhi/rhi.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi_texture.h"
#include "rhi/rhi_types_string.h"
#include "rhi/rhi_cmd_stats.h"
MI_NAMESPACE_BEGIN

RHICommandQueueBase::RHICommandQueueBase() {
    first_command_ = last_command_ = nullptr;
}

RHICommandQueueBase::~RHICommandQueueBase() {

}

void RHICommandQueueBase::PreDestruction() {
    sync_point_.SafeRelease();
}


void RHICommandQueueBase::WaitForIdle (const std::string & submit_prefix, bool host_only) {
    if (host_only) {
         EnqueueTranslateAndSubmit({}, submit_prefix).wait();
    } else {
         auto guard = std::lock_guard(sync_point_mutex_);
         if (!sync_point_) {
             sync_point_ = RHI::Get().CreateSyncPoint();
             sync_point_->SetName(
                 std::format("SyncPoint for RHICommandQueueBase (Type: {})", ToString(GetCommandQueueType()))
             );
         }
         EnqueueTranslateAndSubmit(sync_point_.Raw(), submit_prefix);
         sync_point_->Wait();
         sync_point_->Reset();
     }
}


RHICommandBase::~RHICommandBase() {
    // Do nothing
}

void RHIEmptyCommand::ExecuteAndDestruct([[maybe_unused]] RHICommandQueueBase &cmd) {
    // Do nothing
}


void RHICommandClearTexture::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kClearTexture);
    RHI::Get().GetCommandExecutor()->RHIClearTexture(&cmd, this);
}

void RHICommandClearBuffer::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kClearBuffer);
    RHI::Get().GetCommandExecutor()->RHIClearBuffer(&cmd, this);
}

void RHICommandCopyBufferToTexture::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kCopyBufferToTexture);
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
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kCopyTextureToBuffer);
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
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kCopyBuffer);
    RHI::Get().GetCommandExecutor()->RHICopyBuffer(&cmd, this);
}

void RHICommandCopyTexture::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kCopyTexture);
    RHI::Get().GetCommandExecutor()->RHICopyTexture(&cmd, this);
}

void RHICommandBlitTexture::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBlitTexture);
    RHI::Get().GetCommandExecutor()->RHIBlitTexture(&cmd, this);
}


void RHICommandBeginRendering::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBeginRendering);
    RHI::Get().GetCommandExecutor()->RHIBeginRendering(&cmd, this);
}

void RHICommandEndRendering::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kEndRendering);
    RHI::Get().GetCommandExecutor()->RHIEndRendering(&cmd, this);
}

void RHICommandUpdateDrawState::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kUpdateDrawState);
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}

void RHICommandDraw::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDraw);
    RHI::Get().GetCommandExecutor()->RHIDraw(&cmd, this);
}

void RHICommandDrawIndexed::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDrawIndexed);
    RHI::Get().GetCommandExecutor()->RHIDrawIndexed(&cmd, this);
}

void RHICommandDrawIndirect::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDrawIndirect);
    RHI::Get().GetCommandExecutor()->RHIDrawIndirect(&cmd, this);
}

void RHICommandDrawIndexedIndirect::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDrawIndexedIndirect);
    RHI::Get().GetCommandExecutor()->RHIDrawIndexedIndirect(&cmd, this);
}

void RHICommandSetScissor::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kSetScissor);
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}

void RHICommandSetViewport::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kSetViewport);
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}

void RHICommandSetCullMode::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kSetCullMode);
    RHI::Get().GetCommandExecutor()->RHIUpdateDrawState(&cmd, this);
}


void RHICommandDispatch::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDispatch);
    RHI::Get().GetCommandExecutor()->RHIDispatch(&cmd, this);
}

void RHICommandDispatchIndirect::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDispatchIndirect);
    RHI::Get().GetCommandExecutor()->RHIDispatchIndirect(&cmd, this);
}


void RHICommandBindGraphicsPipeline::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBindGraphicsPipeline);
    RHI::Get().GetCommandExecutor()->RHIBindGraphicsPipeline(&cmd, this);
}

void RHICommandBindComputePipeline::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBindComputePipeline);
    RHI::Get().GetCommandExecutor()->RHIBindComputePipeline(&cmd, this);
}

void RHICommandBindPipelineParameters::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBindPipelineParameters);
    RHI::Get().GetCommandExecutor()->RHIBindPipelineParameters(&cmd, this);
}

void RHICommandBindVertexBuffer::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBindVertexBuffer);
    RHI::Get().GetCommandExecutor()->RHIBindVertexBuffer(&cmd, this);
}

void RHICommandClearBoundState::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kClearBoundState);
    RHI::Get().GetCommandExecutor()->RHIClearBoundState(&cmd, this);
}

void RHICommandMemoryBarrier::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kMemoryBarrier);
    RHI::Get().GetCommandExecutor()->RHIMemoryBarrier(&cmd, this);
}

void RHICommandTextureBarrier::Execute(mi::RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kTextureBarrier);
    RHI::Get().GetCommandExecutor()->RHITextureBarrier(&cmd, this);
}

void RHICommandBufferBarrier::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBufferBarrier);
    RHI::Get().GetCommandExecutor()->RHIBufferBarriers(&cmd, this);
}

void RHICommandAccelerationStructureBarrier::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kAccelerationStructureBarrier);
    RHI::Get().GetCommandExecutor()->RHIAcclerationStructureBarriers(&cmd, this);
}

void RHICommandDebugMarkerBegin::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDebugMarkerBegin);
    RHI::Get().GetCommandExecutor()->RHIDebugMarkerBegin(&cmd, this);
}

void RHICommandDebugMarkerEnd::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDebugMarkerEnd);
    RHI::Get().GetCommandExecutor()->RHIDebugMarkerEnd(&cmd, this);
}

void RHICommandDebugMarkerInsert::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDebugMarkerInsert);
    RHI::Get().GetCommandExecutor()->RHIDebugMarkerInsert(&cmd, this);
}

void RHICommandInsertTimestamp::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kInsertTimestamp);
    RHI::Get().GetCommandExecutor()->RHIInsertTimestamp(&cmd, this);
}

void RHICommandBuildAccelerationStructure::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBuildAccelerationStructure);
    RHI::Get().GetCommandExecutor()->RHIBuildAccelerationStructure(&cmd, this);
}

void RHICommandBindRayTracingPipeline::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBindRayTracingPipeline);
    RHI::Get().GetCommandExecutor()->RHIBindRayTracingPipeline(&cmd, this);
}

void RHICommandDispatchRays::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDispatchRays);
    RHI::Get().GetCommandExecutor()->RHIDispatchRays(&cmd, this);
}

void RHICommandDispatchRaysIndirect::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDispatchRaysIndirect);
    RHI::Get().GetCommandExecutor()->RHIDispatchRaysIndirect(&cmd, this);
}

void RHICommandDispatchRaysIndirect2::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kDispatchRaysIndirect2);
    RHI::Get().GetCommandExecutor()->RHIDispatchRaysIndirect2(&cmd, this);
}

void RHICommandBindShaderBindingTable::Execute(RHICommandQueueBase &cmd) {
    MI_RHI_CMD_STAT_INC(RHICmdStatId::kBindShaderBindingTable);
    RHI::Get().GetCommandExecutor()->RHIBindShaderBindingTable(&cmd, this);
}


MI_NAMESPACE_END
