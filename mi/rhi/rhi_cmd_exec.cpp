/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi_cmd.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

// Command queue instances (Generic)
static RHICommandQueueGraphics * GGraphicsCommandQueue;

void RHICommandQueueGraphics::InitInstance () {
    GGraphicsCommandQueue = new RHICommandQueueGraphics();
}
RHICommandQueueGraphics & RHICommandQueueGraphics::GetInstance() {
    return *GGraphicsCommandQueue;
}


// Implementations for commands
void RHICommandCopyBuffer::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHICopyBuffer(&cmd, this);
}
void RHICommandCopyTexture::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHICopyTexture(&cmd, this);
}
void RHICommandDrawPrimitive::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIDrawPrimitive(&cmd, this);
}
void RHICommandDrawIndexedPrimitive::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIDrawIndexedPrimitive(&cmd, this);
}
void RHICommandDispatch::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIDispatch(&cmd, this);
}
void RHICommandBindGraphicsPipeline::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIBindGraphicsPipeline(&cmd, this);
}
void RHICommandBindComputePipeline::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIBindComputePipeline(&cmd, this);
}
void RHICommandBindPipelineParameters::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIBindPipelineParameters(&cmd, this);
}
void RHICommandBindVertexBuffer::Execute(RHICommandQueueBase &cmd) {
    RHI::GetInstance().GetCommandExecutor()->RHIBindVertexBuffer(&cmd, this);
}


MI_NAMESPACE_END
