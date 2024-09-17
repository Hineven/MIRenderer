/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_cmd.h"
#include "rhi/rhi.h"
#include "rhi_cmd_exec.h"
MI_NAMESPACE_BEGIN

void RHICommandCopyBuffer::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHICopyBuffer(&cmd, this);
}

void RHICommandCopyTexture::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHICopyTexture(&cmd, this);
}

void RHICommandDrawPrimitive::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIDrawPrimitive(&cmd, this);
}

void RHICommandDrawIndexedPrimitive::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIDrawIndexedPrimitive(&cmd, this);
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

void RHICommandFrameEnd::Execute(RHICommandQueueBase &cmd) {
    RHI::Get().GetCommandExecutor()->RHIFrameEnd(&cmd, this);
}

MI_NAMESPACE_END