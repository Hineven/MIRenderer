/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_CMD_EXEC_H
#define MIRENDERER_RHI_CMD_EXEC_H

#include "rhi/rhi_common.h"
#include "rhi/rhi_cmd.h"

MI_NAMESPACE_BEGIN

void SetIsRHIThread (bool is_rhi_thread) ;

// RHICommandExecutor is the interface for actually translating RHI commands.
// Implement this interface in the RHI backend.
class RHICommandExecutorInterface {
public:
    virtual ~RHICommandExecutorInterface() = default;
    virtual void RHICopyBuffer (RHICommandQueueBase * buffer, RHICommandCopyBuffer * cmd) = 0;
    virtual void RHICopyTexture (RHICommandQueueBase * buffer, RHICommandCopyTexture * cmd) = 0;
    virtual void RHIDrawPrimitive (RHICommandQueueBase * buffer, RHICommandDrawPrimitive * cmd) = 0;
    virtual void RHIDrawIndexedPrimitive (RHICommandQueueBase * buffer, RHICommandDrawIndexedPrimitive * cmd) = 0;
    virtual void RHIDispatch (RHICommandQueueBase * buffer, RHICommandDispatch * cmd) = 0;
    virtual void RHIBindGraphicsPipeline (RHICommandQueueBase * buffer, RHICommandBindGraphicsPipeline * cmd) = 0;
    virtual void RHIBindComputePipeline (RHICommandQueueBase * buffer, RHICommandBindComputePipeline * cmd) = 0;
    virtual void RHIBindPipelineParameters (RHICommandQueueBase * buffer, RHICommandBindPipelineParameters * cmd) = 0;
    virtual void RHIBindVertexBuffer (RHICommandQueueBase * buffer, RHICommandBindVertexBuffer * cmd) = 0;
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_CMD_EXEC_H
