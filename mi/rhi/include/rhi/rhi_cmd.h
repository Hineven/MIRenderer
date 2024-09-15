/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_CMD_H
#define MIRENDERERDEV_RHI_CMD_H

#include <format>
#include "core/base.h"
#include "util/alloc.h"
#include "rhi/rhi_common.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_fwd.h"
#include "rhi_worker.h"
#include "rhi_buffer.h"

MI_NAMESPACE_BEGIN

class RHICommandBase {
public:
    virtual ~RHICommandBase() = default;
    // Only called once per object
    virtual void ExecuteAndDestruct (RHICommandQueueBase & cmd) {};
    // Give direct access to the rhi translation thread.
    friend class RHIWorkerThread;
protected:
    RHICommandBase * next_command_ {};
    friend class RHICommandQueueBase;
};

template<typename T>
class TRHILambdaCommand : public RHICommandBase {
public:
    TRHILambdaCommand(T func) : func_(std::move(func)) {}
    void ExecuteAndDestruct (RHICommandQueueBase & cmd) override {
        func_(cmd);
        func_.~T();
    }
private:
    T func_;
};


// RHICommandQueue is a queue of GPU commands. It keeps states about bound pipeline / parameters / resources...
// Commands submitted to the queue go through 3 stages:
// 1. Translation: RHI thread running asynchronously translate the commands into driver commands ready for submission
// 2. Submission: Translated commands are submitted by the RHI thread to device for execution.
// 3. Finished: The command finished execution and is destroyed.
class RHICommandQueueBase : public NonCopyable {
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

    // Queue a lambda function to be executed on the RHI thread
    template<typename T>
    void RHIExecute (T func) {
        auto cmd = AllocateCommand<TRHILambdaCommand<T>>(std::move(func));
        last_command_->next_command_ = cmd;
        last_command_ = cmd;
    }

    // Wait for all commands to finish execution and reset the command buffer,
    // free all temporary memory allocated on the command buffer.
    void Reset () ;

    // Flush existing commands, and send to RHI thread for translation
    // @return a future that will be ready when the translation is completed.
    inline std::future<void> EnqueueTranslation () {
        auto tmp = first_command_;
        first_command_ = last_command_ = AllocateCommand<RHICommandBase>();
        return EnqueueRHICommandTranslationTask(this, tmp);
    }

    // Flush existing commands, and send to RHI thread for baking and submission
    // @param wait_for_device_execution if true, the returned future will wait
    // for the device to finish executing the commands. Otherwise, it will only
    // wait for the host submission completion.
    // @return a future that will be ready when the submission/execution is completed.
    inline std::future<void> SubmitTranslatedCommands (bool wait_for_device_execution = false) {
        return EnqueueRHICommandBufferSubmitTask(this, wait_for_device_execution);
    }

    // Flush existing commands, and send to RHI thread for baking and submission
    // If wait_for_device_execution is true, the task will wait for the device to finish executing the commands
    inline std::future<void> EnqueueTranslateAndSubmit (bool wait_for_device_execution = false) {
        EnqueueTranslation();
        return SubmitTranslatedCommands(wait_for_device_execution);
    }

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

    // Clear and swap allocators. Move on to the next frame.
    // Called by RHI thread
    inline void SwapAllocators_RHIThread () {
        allocator_index_ = 1 - allocator_index_;
        buffer_allocator_[allocator_index_].Reset();
        command_allocator_[allocator_index_].Reset();
    }

protected:

    // Allocate a segment of memory on the command buffer allocator for temporary use.
    // Manually managed command destruction, used internally.
    template<typename T>
    T * AllocateCommand (auto...args) {
        auto ptr = GetCommandAllocator().Allocate(sizeof(T));
        return new(ptr) T(args...);
    }

    void AddCommand (RHICommandBase * cmd) {
        last_command_->next_command_ = cmd;
        last_command_ = cmd;
    }

    int allocator_index_ {0};
    // Used for temporary memory allocation
    TOneTimeLinearAllocator<512 * 1024> buffer_allocator_[2];
    // Used for command allocation
    TOneTimeLinearAllocator<32  * 1024> command_allocator_[2];


    RHICommandBase * first_command_ {};
    RHICommandBase * last_command_ {};

    RHICommandQueueType queue_type_ {RHICommandQueueType::kGraphics};
};

template<typename TCmd>
class TRHICommand : public RHICommandBase
{
public:
    TRHICommand() = default;
    ~TRHICommand() override = default;
    virtual void Execute(RHICommandQueueBase & cmd) = 0;
protected:
    void ExecuteAndDestruct(RHICommandQueueBase & cmd) override {
        TCmd* me = static_cast<TCmd*>(this);
        me->Execute(cmd);
        me->~TCmd();
    }
};

// Command definitions
// TODO i should better isolate their definitions from rhi users
// but im lazy...
class RHICommandCopyBuffer : public TRHICommand<RHICommandCopyBuffer> {
public:
    RHICommandCopyBuffer(RHIBufferSpan src, RHIBufferSpan dst)
        : src_(src), dst_(dst) {}
    void Execute (RHICommandQueueBase & cmd) override ;

    RHIBufferSpan src_;
    RHIBufferSpan dst_;
};

class RHICommandCopyTexture : public TRHICommand<RHICommandCopyTexture> {
public:
    FORCEINLINE RHICommandCopyTexture(
            RHITexture * src, RHITexture * dst,
            int src_x, int src_y, int src_z,
            int dst_x, int dst_y, int dst_z,
            uint32_t width, uint32_t height, uint32_t depth,
            uint32_t src_mip, uint32_t dst_mip,
            uint32_t src_base_layer, uint32_t src_layer_count,
            uint32_t dst_base_layer, uint32_t dst_layer_count)
        : src_(src), dst_(dst),
          src_x_(src_x), src_y_(src_y), src_z_(src_z),
          dst_x_(dst_x), dst_y_(dst_y), dst_z_(dst_z),
          width_(width), height_(height), depth_(depth),
          src_mip_(src_mip), dst_mip_(dst_mip),
          src_base_layer_(src_base_layer), src_layer_count_(src_layer_count),
          dst_base_layer_(dst_base_layer), dst_layer_count_(dst_layer_count) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHITexture * src_;
    RHITexture * dst_;
    int src_x_, src_y_, src_z_;
    int dst_x_, dst_y_, dst_z_;
    uint32_t width_, height_, depth_;
    uint32_t src_mip_, dst_mip_;
    uint32_t src_base_layer_, src_layer_count_;
    uint32_t dst_base_layer_, dst_layer_count_;
};

class RHICommandDrawPrimitive : public TRHICommand<RHICommandDrawPrimitive> {
public:
    RHICommandDrawPrimitive(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
        : vertex_count_(vertex_count), instance_count_(instance_count), first_vertex_(first_vertex), first_instance_(first_instance) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    uint32_t vertex_count_;
    uint32_t instance_count_;
    uint32_t first_vertex_;
    uint32_t first_instance_;
};

class RHICommandDrawIndexedPrimitive : public TRHICommand<RHICommandDrawIndexedPrimitive> {
public:
    RHICommandDrawIndexedPrimitive(RHIBufferSpan index_buffer, uint32_t index_count_,
                                     uint32_t instance_count, uint32_t first_index, uint32_t base_vertex_index,
                                      uint32_t first_instance_index, RHIIndexType index_type)
          : index_buffer_(index_buffer), index_count_(index_count_),
             instance_count_(instance_count), first_index_(first_index),
             base_vertex_index_(base_vertex_index), first_instance_index_(first_instance_index),
             index_type_(index_type) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan index_buffer_;
    uint32_t index_count_;
    uint32_t instance_count_;
    uint32_t first_index_;
    uint32_t base_vertex_index_;
    uint32_t first_instance_index_;
    RHIIndexType index_type_;
};

class RHICommandDispatch : public TRHICommand<RHICommandDispatch> {
public:
    RHICommandDispatch(uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
        : group_count_x_(group_count_x), group_count_y_(group_count_y), group_count_z_(group_count_z) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    uint32_t group_count_x_;
    uint32_t group_count_y_;
    uint32_t group_count_z_;
};

class RHICommandDispatchIndirect : public TRHICommand<RHICommandDispatchIndirect> {
public:
    RHICommandDispatchIndirect(RHIBufferSpan dispatch_command_buffer, uint32_t offset)
        : dispatch_command_buffer_(dispatch_command_buffer), offset_(offset) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan dispatch_command_buffer_;
    uint32_t offset_;
};

class RHICommandBindGraphicsPipeline : public TRHICommand<RHICommandBindGraphicsPipeline> {
public:
    RHICommandBindGraphicsPipeline(RHIGraphicsPipeline * pipeline)
        : pipeline_(pipeline) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIGraphicsPipeline * pipeline_;
};

class RHICommandBindComputePipeline : public TRHICommand<RHICommandBindComputePipeline> {
public:
    RHICommandBindComputePipeline(RHIComputePipeline * pipeline)
        : pipeline_(pipeline) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIComputePipeline * pipeline_;
};

class RHICommandBindPipelineParameters : public TRHICommand<RHICommandBindPipelineParameters> {
public:
    RHICommandBindPipelineParameters(RHIBindPointType point, RHIBindPipelineParametersDesc * table)
        : point_(point), table_(table) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBindPointType point_;
    RHIBindPipelineParametersDesc * table_;
};

class RHICommandBindVertexBuffer : public TRHICommand<RHICommandBindVertexBuffer> {
public:
    RHICommandBindVertexBuffer(uint32_t binding, RHIBufferSpan buffer)
        : binding_(binding), buffer_(buffer) {}
    void Execute(RHICommandQueueBase & cmd) override ;
    RHIBufferSpan buffer_;
    uint32_t binding_;
};

// The first command queue takes care of graphics commands.
class RHICommandQueueGraphics : public RHICommandQueueBase {
protected:
    FORCEINLINE RHICommandQueueGraphics(): RHICommandQueueBase() {
        queue_type_ = RHICommandQueueType::kGraphics;
    }
    static void InitializeSingleton () ;
public:

    static RHICommandQueueGraphics & Get ();

    FORCEINLINE void CopyBuffer (RHIBufferSpan src, RHIBufferSpan dst) {
        AddCommand(AllocateCommand<RHICommandCopyBuffer>(src, dst));
    }

    FORCEINLINE void DrawPrimitive (uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
        AddCommand(AllocateCommand<RHICommandDrawPrimitive>(vertex_count, instance_count, first_vertex, first_instance));
    }
    FORCEINLINE void DrawIndexedPrimitive (RHIBufferSpan index_buffer, uint32_t index_count,
                                          uint32_t instance_count, uint32_t first_index, uint32_t base_vertex_index,
                                          uint32_t first_instance_index, RHIIndexType index_type) {
        AddCommand(AllocateCommand<RHICommandDrawIndexedPrimitive>(index_buffer, index_count, instance_count, first_index, base_vertex_index, first_instance_index, index_type));
    }
    FORCEINLINE void Dispatch (uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
        AddCommand(AllocateCommand<RHICommandDispatch>(group_count_x, group_count_y, group_count_z));
    }
    FORCEINLINE void DispatchIndirect (RHIBufferSpan dispatch_command_buffer, uint32_t offset) {
        AddCommand(AllocateCommand<RHICommandDispatchIndirect>(dispatch_command_buffer, offset));
    }

    // Allocate RHIBindPipelineParameterDesc with the command buffer allocator.
    FORCEINLINE void BindPipelineParameters (RHIBindPointType point, RHIBindPipelineParametersDesc * table) {
        AddCommand(AllocateCommand<RHICommandBindPipelineParameters>(point, table));
    }
};

// No other kinds of command queues are needed for now.


// Mark the resources ready for destruction (unused outside RHI threads), translate & Submit all command queues,
// and after their completion, free resources pending for destruction.
// The future will be ready when all commands are translated, submitted, executed and pending resources are freed.
std::future<void> RHIFlushFrame (RHIFlushFrameBlockingType blocking_type = RHIFlushFrameBlockingType::kNonBlocking) ;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_CMD_H
