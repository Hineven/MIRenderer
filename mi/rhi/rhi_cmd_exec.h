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
    virtual void RHIClearTexture (RHICommandQueueBase * buffer, RHICommandClearTexture * cmd) = 0;
    virtual void RHICopyBufferToTexture (RHICommandQueueBase * buffer, RHICommandCopyBufferToTexture * cmd) = 0;
    virtual void RHICopyTextureToBuffer (RHICommandQueueBase * buffer, RHICommandCopyTextureToBuffer * cmd) = 0;
    virtual void RHICopyBuffer (RHICommandQueueBase * buffer, RHICommandCopyBuffer * cmd) = 0;
    virtual void RHICopyTexture (RHICommandQueueBase * buffer, RHICommandCopyTexture * cmd) = 0;
    virtual void RHIBeginRendering (RHICommandQueueBase * cmd, RHICommandBeginRendering * begin_rendering) = 0;
    virtual void RHIEndRendering (RHICommandQueueBase * cmd, RHICommandEndRendering * end_rendering) = 0;
    virtual void RHIDrawPrimitive (RHICommandQueueBase * buffer, RHICommandDrawPrimitive * cmd) = 0;
    virtual void RHIDrawIndexedPrimitive (RHICommandQueueBase * buffer, RHICommandDrawIndexedPrimitive * cmd) = 0;
    virtual void RHIDrawIndexedIndirect (RHICommandQueueBase * cmd, RHICommandDrawIndexedIndirect * draw_indexed_indirect) = 0;
    virtual void RHIDispatch (RHICommandQueueBase * buffer, RHICommandDispatch * cmd) = 0;
    virtual void RHIBindGraphicsPipeline (RHICommandQueueBase * buffer, RHICommandBindGraphicsPipeline * cmd) = 0;
    // This is the super-set for scissor update, .... etc
    virtual void RHIUpdateDrawState(RHICommandQueueBase * cmd, RHICommandSetScissor * set_scissor) = 0;
    virtual void RHIUpdateDrawState(RHICommandQueueBase * cmd, RHICommandSetViewport * set_viewport) = 0;
    virtual void RHIUpdateDrawState(RHICommandQueueBase * cmd, RHICommandUpdateDrawState * update_draw_state) = 0;
    virtual void RHIBindComputePipeline (RHICommandQueueBase * buffer, RHICommandBindComputePipeline * cmd) = 0;
    virtual void RHIBindPipelineParameters (RHICommandQueueBase * buffer, RHICommandBindPipelineParameters * cmd) = 0;
    virtual void RHIBindVertexBuffer (RHICommandQueueBase * buffer, RHICommandBindVertexBuffer * cmd) = 0;
    virtual void RHITextureBarrier (RHICommandQueueBase * buffer, RHICommandTextureBarrier * cmd) = 0;
    virtual void RHIBufferBarriers (RHICommandQueueBase * buffer, RHICommandBufferBarrier * cmd) = 0;
    virtual void RHIDebugMarkerBegin (RHICommandQueueBase * buffer, RHICommandDebugMarkerBegin * cmd) = 0;
    virtual void RHIDebugMarkerEnd (RHICommandQueueBase * buffer, RHICommandDebugMarkerEnd * cmd) = 0;
    virtual void RHIDebugMarkerInsert (RHICommandQueueBase * buffer, RHICommandDebugMarkerInsert * cmd) = 0;

    // Submit all translated command stored within the command buffer and clear them.
    virtual void RHISubmitCommandBuffer (RHICommandQueueBase * buffer, RHISyncPoint * sync_point, bool recycle_resources) = 0;
    // End the frame, enqueue a present command.
    virtual void RHIFrameEnd (RHICommandQueueBase * buffer, RHISyncPoint * sync) = 0;

};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_CMD_EXEC_H
