/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_CMD_H
#define MIRENDERERDEV_RHI_CMD_H

#include <format>
#include <array>
#include "core/base.h"
#include "core/util/alloc.h"
#include "rhi/rhi_common.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_fwd.h"
#include "rhi_thread.h"
#include "rhi_buffer.h"

MI_NAMESPACE_BEGIN

class RHICommandBase {
public:
    virtual ~RHICommandBase() = default;
    // Only called once per object
    virtual void ExecuteAndDestruct ([[maybe_unused]] RHICommandQueueBase & cmd) {};
    // Give direct access to the rhi translation thread.
    friend class RHIWorkerThread;
protected:
    RHICommandBase * next_command_ {};
    friend class RHICommandQueueBase;
};

class RHICommandQueueBase : public NonCopyable, public NonMovable {
protected:
    inline auto & GetBufferAllocator () {
        return buffer_allocator_[allocator_index_];
    }
    inline auto & GetCommandAllocator () {
        return command_allocator_[allocator_index_];
    }
public:
    RHICommandQueueBase() {
        // Idle command
        first_command_ = last_command_ = AllocateCommand<RHICommandBase>();
    }
    virtual ~RHICommandQueueBase() = default;

    // Wait for all commands to finish execution and reset the command buffer,
    // free all temporary memory allocated on the command buffer.
    void Reset () ;

    // Flush existing commands, and send them to the device
    // @param in_sync_point a sync point that can be waited on for the device to complete executing the submitted commands.
    inline void Submit (RHISyncPoint * in_sync_point = nullptr) ;

    // Allocate a piece of frame local host buffer memory. Very fast linear allocation. Use this
    // function to allocate frame temporaries. The memory will be automatically freed when the command buffer is reset.
    // (That is, when the frame ends.)
    template<CMemTrivial T>
    T * Allocate (auto...args) {
        auto ptr = GetBufferAllocator().Allocate(sizeof(T));
        return new(ptr) T(args...);
    }

    // Allocate a piece of frame local host buffer memory. Very fast linear allocation. Use this
    // function to allocate frame temporaries. The memory will be automatically freed when the command buffer is reset.
    // (That is, when the frame ends.)
    template<CAOUB T>
    std::remove_all_extents_t<T> * Allocate (size_t count) {
        using TElem = std::remove_all_extents_t<T>;
        auto ptr = GetBufferAllocator().Allocate(sizeof(TElem) * count);
        return new(ptr) TElem[count];
    }

    FORCEINLINE RHICommandQueueType GetCommandQueueType () const {return queue_type_;}

protected:

    // Clear and swap allocators. Move on to the next frame.
    // Should be called by RHI implementation.
    inline void SwapAllocators () {
        allocator_index_ = 1 - allocator_index_;
        buffer_allocator_[allocator_index_].Reset();
    }

    int allocator_index_ {0};
    // Used for temporary memory allocation
    TOneTimeLinearAllocator<512 * 1024> buffer_allocator_[2];

    RHICommandQueueType queue_type_ {RHICommandQueueType::kGraphics};
};

// The first command queue takes care of graphics commands.
class RHICommandQueueGraphics : public RHICommandQueueBase {
protected:
    FORCEINLINE RHICommandQueueGraphics(): RHICommandQueueBase() {
        queue_type_ = RHICommandQueueType::kGraphics;
    }
public:
    friend class RHI;
    void ClearTexture (RHITexture * texture, std::array<float, 4> clear_value = {0, 0, 0, 1},
                                   uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t layer_count = 1) ;
    // Unspecified src_image_width and src_image_height assumes that the texels are tightly packed
    // in the buffer
    // Unspecified dst_tex_width, dst_tex_height, dst_tex_depth is the same as the texture's dimensions
    void CopyBufferToTexture (RHIBufferSpan buffer, RHITexture * texture,
                                    uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t layer_count = 1,
                                    uint32_t src_tex_width = 0, uint32_t src_tex_height = 0,
                                    int dst_tex_x = 0, int dst_tex_y = 0, int dst_tex_z = 0,
                                    uint32_t dst_tex_width = 0, uint32_t dst_tex_height = 0, uint32_t dst_tex_depth = 0) ;
    // Unspecified src_image_width and src_image_height assumes that the texels are tightly packed
    // in the buffer
    // Unspecified src_tex_width, src_tex_height, src_tex_depth is the same as the texture's dimensions
    FORCEINLINE void CopyTextureToBuffer (RHITexture * texture, RHIBufferSpan buffer,
                                    uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t layer_count = 1,
                                    uint32_t dst_tex_width = 0, uint32_t dst_tex_height = 0,
                                    int src_tex_x = 0, int src_tex_y = 0, int src_tex_z = 0,
                                    uint32_t src_tex_width = 0, uint32_t src_tex_height = 0, uint32_t src_tex_depth = 0) ;
    FORCEINLINE void CopyBuffer (RHIBufferSpan src, RHIBufferSpan dst) ;

    FORCEINLINE void UpdateDrawState (const RHIDrawDesc & draw_state) ;

    FORCEINLINE void BeginRendering () ;
    FORCEINLINE void EndRendering () ;

    FORCEINLINE void DrawPrimitive (uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex = 0, uint32_t first_instance = 0) ;
    FORCEINLINE void DrawPrimitiveIndexed (RHIBufferSpan index_buffer, uint32_t index_count,
                                          uint32_t instance_count, uint32_t first_index, uint32_t base_vertex_index,
                                          uint32_t first_instance_index, RHIIndexType index_type) ;

    FORCEINLINE void DispatchCompute (uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) ;
    FORCEINLINE void DispatchComputeIndirect (RHIBufferSpan dispatch_command_buffer, uint32_t offset) ;
    // Allocate RHIBindPipelineParameterDesc with the command buffer allocator.
    FORCEINLINE void BindPipelineParameters (RHIBindPointType point, RHIBindPipelineParametersDesc * table) ;

    FORCEINLINE void BindVertexBuffer (uint32_t binding, RHIBufferSpan buffer) ;

    FORCEINLINE void TextureBarrier (
            RHITexture * texture, RHITextureLayoutType layout,
            RHIPipelineStageFlags src_stages, RHIPipelineStageFlags dst_stages,
            RHIGPUAccessFlags src_access, RHIGPUAccessFlags dst_access
    ) ;

    FORCEINLINE void BufferBarrier (
            RHIBufferSpan buffer,
            RHIPipelineStageFlags src_stages, RHIPipelineStageFlags dst_stages,
            RHIGPUAccessFlags src_access, RHIGPUAccessFlags dst_access
    ) ;

    FORCEINLINE void FrameEnd (bool return_resources_to_system) ;

    FORCEINLINE void BindPipeline(RHIGraphicsPipeline * pipeline) ;
    FORCEINLINE void BindPipeline(RHIComputePipeline * pipeline) ;

};

// No other kinds of command queues are needed for now.


// Mark the resources ready for destruction (unused outside RHI threads), translate & Submit all command queues,
// and after their completion, free resources pending for destruction.
// The future will be ready when all commands are translated, submitted, executed and pending resources are freed.
std::future<void> RHIFlushFrame (RHIFlushFrameBlockingType blocking_type = RHIFlushFrameBlockingType::kNonBlocking) ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_CMD_H
