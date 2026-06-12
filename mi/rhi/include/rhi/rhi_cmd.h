/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_CMD_H
#define MIRENDERERDEV_RHI_CMD_H

#include <format>
#include <array>
#include <cstring>
#include <stack>

#include "rhi_desc.h"
#include "rhi_as_types.h"
#include "core/base.h"
#include "core/util/alloc.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_fwd.h"
#include "rhi_thread.h"
#include "core/util/debug_prof.h"
#include "rhi/rhi_cmd_stats.h"

MI_NAMESPACE_BEGIN

class RHICommandBase {
public:
    virtual ~RHICommandBase() ;
    // Only called once per object
    virtual void ExecuteAndDestruct ([[maybe_unused]] RHICommandQueueBase & cmd) = 0;
    // Give direct access to the rhi translation thread.
    friend class RHIWorkerThread;
protected:
    RHICommandBase * next_command_ {};
    friend class RHICommandQueueBase;
};

class RHIEmptyCommand : public RHICommandBase {
public:
    void ExecuteAndDestruct (RHICommandQueueBase & cmd) override ;
};

template<typename T>
class TRHILambdaCommand : public RHICommandBase {
public:
    TRHILambdaCommand(T func) : func_(std::move(func)) {}
    void ExecuteAndDestruct (RHICommandQueueBase & cmd) override {
        MI_RHI_CMD_STAT_INC(RHICmdStatId::kLambda);
        func_(cmd);
        this->~TRHILambdaCommand();
    }
private:
    T func_;
};

template<typename T>
concept CRHIValidCommand = std::derived_from<T, RHICommandBase>;

// RHICommandQueue is a queue of GPU commands. It keeps states about bound pipeline / parameters / resources...
// Commands submitted to the queue go through 3 stages:
// 1. Translation: RHI thread running asynchronously translate the commands into driver commands ready for submission
// 2. Submission: Translated commands are submitted by the RHI thread to device for execution.
// 3. Finished: The command finished execution and is destroyed.
class RHICommandQueueBase : public NonCopyable, public NonMovable {
protected:
    inline auto & GetBufferAllocator () {
        return buffer_allocator_[allocator_index_];
    }
    inline auto & GetCommandAllocator () {
        return command_allocator_[allocator_index_];
    }
public:
    RHICommandQueueBase() ;
    virtual ~RHICommandQueueBase() ;

    // Queue a lambda function to be executed on the RHI thread
    template<typename T>
    void RHIExecute (T func) {
        auto cmd = AllocateCommand<TRHILambdaCommand<T>>(std::move(func));
        AddCommand(cmd);
    }

    // Flush existing commands, and send to RHI thread for translation
    // @return a future that will be ready when the translation is completed.
    FORCEINLINE std::future<void> EnqueueTranslation () {
        auto tmp = first_command_;
        first_command_ = last_command_ = nullptr;
        return EnqueueRHICommandTranslationTask(this, tmp);
    }

    // Flush existing commands, and send to RHI thread for baking and submission
    // @param in_sync_point a sync point that can be waited on for the device to complete executing the submitted commands.
    // @param recycle_resources whether to recycle translated commands immediately after submission rather than in
    // frame intervals. May cause overhead.
    // @return a future that will be ready when the submission is completed.
    FORCEINLINE std::future<void> SubmitTranslatedCommands (
            RHISyncPoint * in_sync_point = nullptr,
            const std::string & submit_prefix = "",
            bool recycle_resources = false) {
        return EnqueueRHICommandBufferSubmitTask(this, in_sync_point, submit_prefix, recycle_resources);
    }

    // Flush existing commands, and send to RHI thread for baking and submission
    // @param in_sync_point a sync point that can be waited on for the device to complete executing the submitted commands.
    // @param recycle_resources whether to recycle translated commands immediately after submission rather than in
    // frame intervals. May cause overhead.
    // @return a future that will be ready when the submission is completed.
    FORCEINLINE std::future<void> EnqueueTranslateAndSubmit (
        RHISyncPoint * in_sync_point = nullptr,
        const std::string & submit_prefix = "",
        bool recycle_resources = false) {
        EnqueueTranslation();
        return SubmitTranslatedCommands(in_sync_point, submit_prefix, recycle_resources);
    }

    // Shortcut.
    // Flush the queue and wait for all commands to finish execution on the device.
    void WaitForIdle (const std::string & submit_prefix = "", bool host_only = false) ;

    // End the frame, enqueue a present command, and return resources to the system if requested.
    // The command is special, it does not require submission to execute. Translation will be enough.
    // @param sync_point: the fence to set when the frame finished presenting.
    FORCEINLINE void FrameEnd (RHISyncPoint * sync_point) {
        EnqueueTranslation();
        EnqueueRHIFrameEndTask(this, sync_point);
    }

    // Allocate raw memory
    FORCEINLINE void * AllocateRaw (size_t size) {
        assert(IsRenderThread());
        return GetBufferAllocator().Allocate(size);
    }

    // Allocate a piece of frame local host buffer memory. Very fast linear allocation. Use this
    // function to allocate frame temporaries. The memory will be automatically freed when the command buffer is reset.
    // (That is, when the frame ends.)
    template<CMemTrivial T>
    T * Allocate (auto...args) {
        assert(IsRenderThread());
        auto ptr = GetBufferAllocator().Allocate(sizeof(T));
        return new(ptr) T(args...);
    }

    // Allocate a piece of frame local host buffer memory. Very fast linear allocation. Use this
    // function to allocate frame temporaries. The memory will be automatically freed when the command buffer is reset.
    // (That is, when the frame ends.)
    template<CAOUB T>
    std::remove_all_extents_t<T> * Allocate (size_t count) {
        assert(IsRenderThread());
        using TElem = std::remove_all_extents_t<T>;
        auto ptr = GetBufferAllocator().Allocate(sizeof(TElem) * count);
        return new(ptr) TElem[count];
    }

    FORCEINLINE RHICommandQueueType GetCommandQueueType () const {return queue_type_;}

    // Clear and swap allocators. Move on to the next frame.
    FORCEINLINE void SwapAllocators () {
        allocator_index_ = 1 - allocator_index_;
        buffer_allocator_[allocator_index_].Reset();
        command_allocator_[allocator_index_].Reset();
    }

protected:
    // Called before the destruction of RHI.
    void PreDestruction ();

    // Allocate a segment of memory on the command buffer allocator for temporary use.
    // Manually managed command destruction, used internally.
    template<CRHIValidCommand T>
    T * AllocateCommand (auto...args) {
        assert(IsRenderThread());
        auto ptr = GetCommandAllocator().Allocate(sizeof(T));
        return new(ptr) T(args...);
    }

    void AddCommand (RHICommandBase * cmd) {
        if constexpr (MI_BYPASS_RHI_THREAD) {
            DEBUG_PROFILE_SECTION(AddCommand);
            // If we are bypassing the RHI thread, execute the command immediately.
            cmd->ExecuteAndDestruct(*this);
        } else {
            if (!first_command_) {
                first_command_ = last_command_ = AllocateCommand<RHIEmptyCommand>();
            }
            last_command_->next_command_ = cmd;
            last_command_ = cmd;
        }
    }

    int allocator_index_ {0};
    // Used for temporary memory allocation
    TOneTimeLinearAllocator<512 * 1024> buffer_allocator_[2];
    // Used for command allocation
    TOneTimeLinearAllocator<32  * 1024> command_allocator_[2];


    RHICommandBase * first_command_ {};
    RHICommandBase * last_command_ {};

    RHICommandQueueType queue_type_ {RHICommandQueueType::kGraphics};

    // Resources used by WaitForIdle().
    std::mutex sync_point_mutex_;
    TRef<RHISyncPoint> sync_point_;
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
class RHICommandClearTexture : public TRHICommand<RHICommandClearTexture> {
public:
    RHICommandClearTexture(RHITexture * texture, const std::array<float, 4> & clear_value,
                           uint32_t mip_level, uint32_t base_layer, uint32_t layer_count)
    : texture_(texture), clear_value_(clear_value), mip_level_(mip_level),
          base_layer_(base_layer), layer_count_(layer_count) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHITexture * texture_;
    std::array<float, 4> clear_value_;
    uint32_t mip_level_;
    uint32_t base_layer_;
    uint32_t layer_count_;
};

class RHICommandClearBuffer : public TRHICommand<RHICommandClearBuffer> {
public:
    RHICommandClearBuffer(RHIBufferSpan buffer, uint32_t clear_value)
        : buffer_(buffer), clear_value_(clear_value) {}
    void Execute(RHICommandQueueBase & cmd) override ;
    RHIBufferSpan buffer_;
    uint32_t clear_value_;
};

class RHICommandCopyBufferToTexture : public TRHICommand<RHICommandCopyBufferToTexture> {
public:
    RHICommandCopyBufferToTexture(RHIBufferSpan buffer, RHITexture * texture,
                                  uint32_t mip_level, uint32_t base_layer, uint32_t layer_count,
                                  uint32_t src_tex_width, uint32_t src_tex_height,
                                  int dst_tex_x, int dst_tex_y, int dst_tex_z,
                                  uint32_t dst_tex_width, uint32_t dst_tex_height, uint32_t dst_tex_depth)
        : buffer_(buffer), texture_(texture), mip_level_(mip_level),
          base_layer_(base_layer), layer_count_(layer_count),
          src_tex_width_(src_tex_width), src_tex_height_(src_tex_height),
          dst_tex_x_(dst_tex_x), dst_tex_y_(dst_tex_y), dst_tex_z_(dst_tex_z),
          dst_tex_width_(dst_tex_width), dst_tex_height_(dst_tex_height), dst_tex_depth_(dst_tex_depth){}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan buffer_;
    RHITexture * texture_;
    uint32_t mip_level_;
    uint32_t base_layer_;
    uint32_t layer_count_;
    uint32_t src_tex_width_; // texels
    uint32_t src_tex_height_; // texels
    int      dst_tex_x_;
    int      dst_tex_y_;
    int      dst_tex_z_;
    uint32_t dst_tex_width_;
    uint32_t dst_tex_height_;
    uint32_t dst_tex_depth_;
};

class RHICommandCopyTextureToBuffer : public TRHICommand<RHICommandCopyTextureToBuffer> {
public:
    RHICommandCopyTextureToBuffer(RHITexture * texture, RHIBuffer * buffer, size_t buffer_offset,
                                  uint32_t mip_level, uint32_t base_layer, uint32_t layer_count,
                                  uint32_t dst_tex_width, uint32_t dst_tex_height,
                                  int src_tex_x, int src_tex_y, int src_tex_z,
                                  uint32_t src_tex_width, uint32_t src_tex_height, uint32_t src_tex_depth)
        : texture_(texture), buffer_(buffer), buffer_offset_(buffer_offset), mip_level_(mip_level),
          base_layer_(base_layer), layer_count_(layer_count),
          dst_tex_width_(dst_tex_width), dst_tex_height_(dst_tex_height),
          src_tex_x_(src_tex_x), src_tex_y_(src_tex_y), src_tex_z_(src_tex_z),
          src_tex_width_(src_tex_width), src_tex_height_(src_tex_height), src_tex_depth_(src_tex_depth) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHITexture * texture_;
    RHIBuffer * buffer_;
    size_t buffer_offset_;
    uint32_t mip_level_;
    uint32_t base_layer_;
    uint32_t layer_count_;
    uint32_t dst_tex_width_; // texels
    uint32_t dst_tex_height_; // texels
    int      src_tex_x_;
    int      src_tex_y_;
    int      src_tex_z_;
    uint32_t src_tex_width_;
    uint32_t src_tex_height_;
    uint32_t src_tex_depth_;
};

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
          width_(width), height_(height), depth_(depth), // 0 means full size
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

class RHICommandBlitTexture : public TRHICommand<RHICommandBlitTexture> {
public:
    FORCEINLINE RHICommandBlitTexture(
            RHITexture * src, RHITexture * dst,
            int src_x, int src_y, int src_z,
            int src_end_x, int src_end_y, int src_end_z, // Exclusive
            int dst_x, int dst_y, int dst_z,
            int dst_end_x, int dst_end_y, int dst_end_z, // Exclusive
            uint32_t src_mip, uint32_t dst_mip,
            uint32_t src_base_layer, uint32_t src_layer_count,
            uint32_t dst_base_layer, uint32_t dst_layer_count,
            RHISamplerFilterType filter)
        : src_(src), dst_(dst),
          src_x_(src_x), src_y_(src_y), src_z_(src_z),
            src_end_x_(src_end_x), src_end_y_(src_end_y), src_end_z_(src_end_z),
          dst_x_(dst_x), dst_y_(dst_y), dst_z_(dst_z),
            dst_end_x_(dst_end_x), dst_end_y_(dst_end_y), dst_end_z_(dst_end_z),
          src_mip_(src_mip), dst_mip_(dst_mip),
          src_base_layer_(src_base_layer), src_layer_count_(src_layer_count),
          dst_base_layer_(dst_base_layer), dst_layer_count_(dst_layer_count),
            filter_(filter) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHITexture * src_;
    RHITexture * dst_;
    int src_x_, src_y_, src_z_;
    int src_end_x_, src_end_y_, src_end_z_;
    int dst_x_, dst_y_, dst_z_;
    int dst_end_x_, dst_end_y_, dst_end_z_;
    uint32_t src_mip_, dst_mip_;
    uint32_t src_base_layer_, src_layer_count_;
    uint32_t dst_base_layer_, dst_layer_count_;
    RHISamplerFilterType filter_;
};

class RHICommandBeginRendering : public TRHICommand<RHICommandBeginRendering> {
public:
    RHICommandBeginRendering() {}
    void Execute(RHICommandQueueBase & cmd) override ;
};

class RHICommandEndRendering : public TRHICommand<RHICommandEndRendering> {
public:
    RHICommandEndRendering() {}
    void Execute(RHICommandQueueBase & cmd) override ;
};

class RHICommandUpdateDrawState : public TRHICommand<RHICommandUpdateDrawState> {
public:
    RHICommandUpdateDrawState(const RHIDrawStateDesc & draw_state)
        : draw_state_(draw_state) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIDrawStateDesc draw_state_;
};

class RHICommandDraw : public TRHICommand<RHICommandDraw> {
public:
    RHICommandDraw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
        : vertex_count_(vertex_count), instance_count_(instance_count), first_vertex_(first_vertex), first_instance_(first_instance) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    uint32_t vertex_count_;
    uint32_t instance_count_;
    uint32_t first_vertex_;
    uint32_t first_instance_;
};

class RHICommandSetScissor : public TRHICommand<RHICommandSetScissor> {
public:
    RHICommandSetScissor(int x, int y, uint32_t width, uint32_t height)
        : x_(x), y_(y), width_(width), height_(height) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    int x_;
    int y_;
    uint32_t width_;
    uint32_t height_;
};

class RHICommandSetViewport : public TRHICommand<RHICommandSetViewport> {
public:
    RHICommandSetViewport(float x, float y, float width, float height, float min_depth, float max_depth)
        : x_(x), y_(y), width_(width), height_(height), min_depth_(min_depth), max_depth_(max_depth) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    float x_;
    float y_;
    float width_;
    float height_;
    float min_depth_;
    float max_depth_;
};

class RHICommandSetCullMode : public TRHICommand<RHICommandSetCullMode> {
public:
    RHICommandSetCullMode(RHICullModeType cull_mode): cull_mode_(cull_mode) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHICullModeType cull_mode_;
};

class RHICommandDrawIndexed : public TRHICommand<RHICommandDrawIndexed> {
public:
    RHICommandDrawIndexed(RHIBufferSpan index_buffer, uint32_t index_count_,
                                     uint32_t instance_count, uint32_t first_index, int base_vertex_index,
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
    int      base_vertex_index_;
    uint32_t first_instance_index_;
    RHIIndexType index_type_;
};

class RHICommandDrawIndirect : public TRHICommand<RHICommandDrawIndirect> {
public:
    RHICommandDrawIndirect(RHIBufferSpan command, uint32_t count)
        : command_(command), count_(count) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan command_;
    uint32_t count_;
};

class RHICommandDrawIndexedIndirect : public TRHICommand<RHICommandDrawIndexedIndirect> {
public:
    RHICommandDrawIndexedIndirect(RHIBufferSpan index_buffer, RHIBufferSpan indirect_buffer, uint32_t draw_count, RHIIndexType index_type):
        index_buffer_(index_buffer), indirect_buffer_(indirect_buffer), draw_count_(draw_count), index_type_(index_type) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan index_buffer_;
    RHIBufferSpan indirect_buffer_;
    uint32_t draw_count_;
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
    RHICommandDispatchIndirect(RHIBuffer * dispatch_command_buffer, uint32_t offset)
        : dispatch_command_buffer_(dispatch_command_buffer), offset_(offset) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBuffer * dispatch_command_buffer_;
    uint32_t offset_;
};

class RHICommandBindGraphicsPipeline : public TRHICommand<RHICommandBindGraphicsPipeline> {
public:
    RHICommandBindGraphicsPipeline(RHIGraphicsPipeline * pipeline, RHIPipelineRootSignature * root_signature)
        : pipeline_(pipeline), root_signature_(root_signature) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIGraphicsPipeline * pipeline_;
    RHIPipelineRootSignature * root_signature_;
};

class RHICommandBindComputePipeline : public TRHICommand<RHICommandBindComputePipeline> {
public:
    RHICommandBindComputePipeline(RHIComputePipeline * pipeline, RHIPipelineRootSignature * root_signature)
        : pipeline_(pipeline), root_signature_(root_signature) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIComputePipeline * pipeline_;
    RHIPipelineRootSignature * root_signature_;
};

// Create-info for one signature parameter table. The descriptor arrays inside `desc` point into
// frame-local storage allocated by the command buffer; this struct is trivially copyable.
struct RHISignatureParameterTableCreateInfo {
    RHIPipelineRootSignature * root_signature;
    RHIBindPipelineParametersDesc desc;
};

// Batch-create N signature parameter tables whose ids are contiguous: base, base+1, ..., base+N-1.
// Mirrors the barrier batch command idiom (RHICommandBufferBarrier): count + raw pointer to a
// frame-local array of create-infos. On the Vulkan backend the per-table descriptor set allocations
// and descriptor writes are amortized into a single vkAllocateDescriptorSets + vkUpdateDescriptorSets.
class RHICommandCreateSignatureParameterTables : public TRHICommand<RHICommandCreateSignatureParameterTables> {
public:
    RHICommandCreateSignatureParameterTables(
        uint32_t base_table_id, uint32_t count, RHISignatureParameterTableCreateInfo * tables)
        : base_table_id_(base_table_id), count_(count), tables_(tables) {}
    void Execute(RHICommandQueueBase & cmd) override;

    uint32_t base_table_id_;
    uint32_t count_;
    RHISignatureParameterTableCreateInfo * tables_;
};

class RHICommandBindSignatureParameterTable : public TRHICommand<RHICommandBindSignatureParameterTable> {
public:
    RHICommandBindSignatureParameterTable(
        uint32_t table_id,
        RHIBindPointType point)
        : table_id_(table_id), point_(point) {}
    void Execute(RHICommandQueueBase & cmd) override;

    uint32_t table_id_;
    RHIBindPointType point_;
};

class RHICommandBindVertexBuffer : public TRHICommand<RHICommandBindVertexBuffer> {
public:
    RHICommandBindVertexBuffer(uint32_t binding, RHIBufferSpan buffer)
        : binding_(binding), buffer_(buffer) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan buffer_;
    uint32_t binding_;
};

// Push a block of push constant data to the currently bound pipeline on the given bind point.
// BindPipeline must have been called for that bind point before this command executes.
// The data span backing memory is owned by the command (copied at recording time).
// stages narrows which shader stages receive the data (avoids the cost of pushing to all stages).
// It must be a subset of the stages declared in the pipeline layout's push constant range.
class RHICommandPushConstants : public TRHICommand<RHICommandPushConstants> {
public:
    RHICommandPushConstants(std::span<std::byte> data, RHIBindPointType point, RHIShaderFrequencyFlags stages)
        : data_(data), point_(point), stages_(stages) {}
    void Execute(RHICommandQueueBase & cmd) override;

    std::span<std::byte> data_;
    RHIBindPointType point_;
    RHIShaderFrequencyFlags stages_;
};

class RHICommandMemoryBarrier : public TRHICommand<RHICommandMemoryBarrier> {
public:
    RHICommandMemoryBarrier(RHIPipelineStageFlags src_stages, RHIPipelineStageFlags dst_stages,
                            RHIGPUAccessFlags src_accesses, RHIGPUAccessFlags dst_accesses)
        : src_stages_(src_stages), dst_stages_(dst_stages),
          src_accesses_(src_accesses), dst_accesses_(dst_accesses) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIPipelineStageFlags src_stages_;
    RHIPipelineStageFlags dst_stages_;
    RHIGPUAccessFlags src_accesses_;
    RHIGPUAccessFlags dst_accesses_;
};

class RHICommandTextureBarrier : public TRHICommand<RHICommandTextureBarrier> {
public:
    RHICommandTextureBarrier(
            uint32_t num_textures,
            RHITexture ** textures, RHITextureLayoutType * layouts,
            RHIPipelineStageFlags * src_stages, RHIPipelineStageFlags * dst_stages,
            RHIGPUAccessFlags * src_accesses, RHIGPUAccessFlags * dst_accesses
    ): num_textures_(num_textures),
    textures_(textures), layouts_(layouts), src_stages_(src_stages), dst_stages_(dst_stages),
    src_accesses_(src_accesses), dst_accesses_(dst_accesses) {}
    void Execute(RHICommandQueueBase & cmd) override ;
    uint32_t num_textures_;
    RHITexture ** textures_;
    RHITextureLayoutType * layouts_;
    RHIPipelineStageFlags * src_stages_;
    RHIPipelineStageFlags * dst_stages_;
    RHIGPUAccessFlags * src_accesses_;
    RHIGPUAccessFlags * dst_accesses_;
};

class RHICommandBufferBarrier : public TRHICommand<RHICommandBufferBarrier> {
public:
    RHICommandBufferBarrier(
            uint32_t num_buffers,
            RHIBufferSpan * buffers,
            RHIPipelineStageFlags * src_stages, RHIPipelineStageFlags * dst_stages,
            RHIGPUAccessFlags * src_accesses, RHIGPUAccessFlags * dst_accesses
    ): num_buffers_(num_buffers),
        buffers_(buffers), src_stages_(src_stages), dst_stages_(dst_stages),
        src_accesses_(src_accesses), dst_accesses_(dst_accesses) {}
    void Execute(RHICommandQueueBase & cmd) override ;
    uint32_t num_buffers_;
    RHIBufferSpan * buffers_;
    RHIPipelineStageFlags * src_stages_;
    RHIPipelineStageFlags * dst_stages_;
    RHIGPUAccessFlags * src_accesses_;
    RHIGPUAccessFlags * dst_accesses_;
};

class RHICommandBarriers : public TRHICommand<RHICommandBarriers> {
public:
    RHICommandBarriers(
        uint32_t num_textures,
        RHITexture ** textures, RHITextureLayoutType * layouts,
        RHIPipelineStageFlags * tex_src_stages, RHIPipelineStageFlags * tex_dst_stages,
        RHIGPUAccessFlags * tex_src_accesses, RHIGPUAccessFlags * tex_dst_accesses,
        uint32_t num_buffers,
        RHIBufferSpan * buffers,
        RHIPipelineStageFlags * buf_src_stages, RHIPipelineStageFlags * buf_dst_stages,
        RHIGPUAccessFlags * buf_src_accesses, RHIGPUAccessFlags * buf_dst_accesses
    ): num_textures_(num_textures),
        textures_(textures), layouts_(layouts),
        tex_src_stages_(tex_src_stages), tex_dst_stages_(tex_dst_stages),
        tex_src_accesses_(tex_src_accesses), tex_dst_accesses_(tex_dst_accesses),
        num_buffers_(num_buffers), buffers_(buffers),
        buf_src_stages_(buf_src_stages), buf_dst_stages_(buf_dst_stages),
        buf_src_accesses_(buf_src_accesses), buf_dst_accesses_(buf_dst_accesses) {}
    void Execute(RHICommandQueueBase & cmd) override ;
    uint32_t num_textures_;
    RHITexture ** textures_;
    RHITextureLayoutType * layouts_;
    RHIPipelineStageFlags * tex_src_stages_;
    RHIPipelineStageFlags * tex_dst_stages_;
    RHIGPUAccessFlags * tex_src_accesses_;
    RHIGPUAccessFlags * tex_dst_accesses_;
    uint32_t num_buffers_;
    RHIBufferSpan * buffers_;
    RHIPipelineStageFlags * buf_src_stages_;
    RHIPipelineStageFlags * buf_dst_stages_;
    RHIGPUAccessFlags * buf_src_accesses_;
    RHIGPUAccessFlags * buf_dst_accesses_;
};

class RHICommandAccelerationStructureBarrier : public TRHICommand<RHICommandAccelerationStructureBarrier> {
public:
    RHICommandAccelerationStructureBarrier(
        uint32_t num_barriers,
        RHIAccelerationStructure ** acceleration_structures,
        RHIPipelineStageFlags * src_stages, RHIPipelineStageFlags * dst_stages,
        RHIGPUAccessFlags * src_accesses, RHIGPUAccessFlags * dst_accesses
    ): num_barriers_(num_barriers),
        acceleration_structures_(acceleration_structures), src_stages_(src_stages), dst_stages_(dst_stages),
        src_accesses_(src_accesses), dst_accesses_(dst_accesses) {}
    void Execute(RHICommandQueueBase & cmd) override ;
    uint32_t num_barriers_;
    RHIAccelerationStructure ** acceleration_structures_;
    RHIPipelineStageFlags * src_stages_;
    RHIPipelineStageFlags * dst_stages_;
    RHIGPUAccessFlags * src_accesses_;
    RHIGPUAccessFlags * dst_accesses_;
};

class RHICommandDebugMarkerBegin : public TRHICommand<RHICommandDebugMarkerBegin> {
public:
    RHICommandDebugMarkerBegin(const char* marker_name, const std::array<float, 4>& color = {1.0f, 1.0f, 1.0f, 1.0f})
            : marker_name_(marker_name), color_(color) {}
    void Execute(RHICommandQueueBase & cmd) override;

    const char* marker_name_;
    std::array<float, 4> color_;
};

class RHICommandDebugMarkerEnd : public TRHICommand<RHICommandDebugMarkerEnd> {
public:
    RHICommandDebugMarkerEnd() = default;
    void Execute(RHICommandQueueBase & cmd) override;
};

class RHICommandDebugMarkerInsert : public TRHICommand<RHICommandDebugMarkerInsert> {
public:
    RHICommandDebugMarkerInsert(const char* marker_name, const std::array<float, 4>& color = {1.0f, 1.0f, 1.0f, 1.0f})
            : marker_name_(marker_name), color_(color) {}
    void Execute(RHICommandQueueBase & cmd) override;

    const char* marker_name_;
    std::array<float, 4> color_;
};

// Insert a GPU timestamp into the global query pool for later readback.
class RHICommandInsertTimestamp : public TRHICommand<RHICommandInsertTimestamp> {
public:
    RHICommandInsertTimestamp(RHITimestamp* timestamp, RHIPipelineStageFlagBits stage)
        : timestamp_(timestamp), stage_(stage) {}
    void Execute(RHICommandQueueBase & cmd) override;

    RHITimestamp* timestamp_;
    RHIPipelineStageFlagBits stage_;
};

// Ray tracing commands
class RHICommandBuildAccelerationStructure : public TRHICommand<RHICommandBuildAccelerationStructure> {
public:
    RHICommandBuildAccelerationStructure(
        const RHIAccelerationStructureBuildGeometryInfo& build_info,
        RHIBufferSpan scratch_buffer)
        : build_info_(build_info), scratch_buffer_(scratch_buffer) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIAccelerationStructureBuildGeometryInfo build_info_;
    RHIBufferSpan scratch_buffer_;
};

class RHICommandBindRayTracingPipeline : public TRHICommand<RHICommandBindRayTracingPipeline> {
public:
    RHICommandBindRayTracingPipeline(RHIRayTracingPipeline * pipeline, RHIPipelineRootSignature * root_signature)
        : pipeline_(pipeline), root_signature_(root_signature) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIRayTracingPipeline * pipeline_;
    RHIPipelineRootSignature * root_signature_;
};

class RHICommandBindShaderBindingTable : public TRHICommand<RHICommandBindShaderBindingTable> {
public:
    RHICommandBindShaderBindingTable(
        RHIBufferSpan raygen_sbt,
        RHIBufferSpan miss_sbt,
        RHIBufferSpan hit_sbt,
        RHIBufferSpan callable_sbt)
        : raygen_sbt_(raygen_sbt), miss_sbt_(miss_sbt),
          hit_sbt_(hit_sbt), callable_sbt_(callable_sbt) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan raygen_sbt_;
    RHIBufferSpan miss_sbt_;
    RHIBufferSpan hit_sbt_;
    RHIBufferSpan callable_sbt_;
};

class RHICommandDispatchRays : public TRHICommand<RHICommandDispatchRays> {
public:
    RHICommandDispatchRays(uint32_t width, uint32_t height, uint32_t depth)
        : width_(width), height_(height), depth_(depth) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    uint32_t width_;
    uint32_t height_;
    uint32_t depth_;
};

class RHICommandDispatchRaysIndirect : public TRHICommand<RHICommandDispatchRaysIndirect> {
public:
    RHICommandDispatchRaysIndirect(
        RHIBufferSpan raygen,
        RHIBufferSpan miss, uint64_t miss_stride,
        RHIBufferSpan hit, uint64_t hit_stride,
        RHIBufferSpan indirect_buffer
    ): raygen_(raygen), miss_(miss), miss_stride_(miss_stride), hit_(hit), hit_stride_(hit_stride),
        indirect_buffer_(indirect_buffer) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan raygen_;
    RHIBufferSpan miss_;
    uint64_t miss_stride_;
    RHIBufferSpan hit_;
    uint64_t hit_stride_;
    RHIBufferSpan indirect_buffer_;
};

class RHICommandDispatchRaysIndirect2 : public TRHICommand<RHICommandDispatchRaysIndirect2> {
public:
    RHICommandDispatchRaysIndirect2(RHIBufferSpan indirect_buffer)
        : indirect_buffer_(indirect_buffer) {}
    void Execute(RHICommandQueueBase & cmd) override ;

    RHIBufferSpan indirect_buffer_;
};

// The first command queue takes care of graphics commands.
class RHICommandQueueGraphics : public RHICommandQueueBase {
protected:
    FORCEINLINE RHICommandQueueGraphics(): RHICommandQueueBase() {
        queue_type_ = RHICommandQueueType::kGraphics;
    }
public:
    friend class RHI;
    // For uint textures, use std::bit_cast<float> to convert uint values to float clear values
    FORCEINLINE void ClearTexture (RHITexture * texture, std::array<float, 4> clear_value = {0, 0, 0, 1},
                                   uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t layer_count = 1) {
        AddCommand(AllocateCommand<RHICommandClearTexture>(texture, clear_value, mip_level, base_layer, layer_count));
    }
    FORCEINLINE void ClearBuffer (RHIBufferSpan buffer, uint32_t clear_value = 0) {
        AddCommand(AllocateCommand<RHICommandClearBuffer>(buffer, clear_value));
    }

    FORCEINLINE void CopyTexture (RHITexture * src, RHITexture * dst,
                             int src_x = 0, int src_y = 0, int src_z = 0,
                             int dst_x = 0, int dst_y = 0, int dst_z = 0,
                             uint32_t width = 0, uint32_t height = 0, uint32_t depth = 1, // 0 means full size
                             uint32_t src_mip = 0, uint32_t dst_mip = 0,
                             uint32_t src_base_layer = 0, uint32_t src_layer_count = 1,
                             uint32_t dst_base_layer = 0, uint32_t dst_layer_count = 1) {
        AddCommand(AllocateCommand<RHICommandCopyTexture>(
                src, dst,
                src_x, src_y, src_z,
                dst_x, dst_y, dst_z,
                width, height, depth,
                src_mip, dst_mip,
                src_base_layer, src_layer_count,
                dst_base_layer, dst_layer_count));
    }

    FORCEINLINE void BlitTexture (RHITexture * src, RHITexture * dst,
                             int src_x, int src_y, int src_z,
                             int src_end_x, int src_end_y, int src_end_z, // Exclusive
                             int dst_x, int dst_y, int dst_z,
                             int dst_end_x, int dst_end_y, int dst_end_z, // Exclusive
                             uint32_t src_mip = 0, uint32_t dst_mip = 0,
                             uint32_t src_base_layer = 0, uint32_t src_layer_count = 1,
                             uint32_t dst_base_layer = 0, uint32_t dst_layer_count = 1,
                             RHISamplerFilterType filter = RHISamplerFilterType::kLinear) {
        AddCommand(AllocateCommand<RHICommandBlitTexture>(
                src, dst,
                src_x, src_y, src_z,
                src_end_x, src_end_y, src_end_z,
                dst_x, dst_y, dst_z,
                dst_end_x, dst_end_y, dst_end_z,
                src_mip, dst_mip,
                src_base_layer, src_layer_count,
                dst_base_layer, dst_layer_count,
                filter));
    }

    // Unspecified src_image_width and src_image_height assumes that the texels are tightly packed
    // in the buffer
    // Unspecified dst_tex_width, dst_tex_height, dst_tex_depth is the same as the texture's dimensions
    FORCEINLINE void CopyBufferToTexture (RHIBufferSpan buffer, RHITexture * texture,
                                    uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t layer_count = 1,
                                    uint32_t src_tex_width = 0, uint32_t src_tex_height = 0,
                                    int dst_tex_x = 0, int dst_tex_y = 0, int dst_tex_z = 0,
                                    uint32_t dst_tex_width = 0, uint32_t dst_tex_height = 0, uint32_t dst_tex_depth = 0) {
        AddCommand(AllocateCommand<RHICommandCopyBufferToTexture>(
                buffer, texture, mip_level, base_layer, layer_count,
                src_tex_width, src_tex_height,
                dst_tex_x, dst_tex_y, dst_tex_z,
                dst_tex_width, dst_tex_height, dst_tex_depth));
    }
    // Unspecified src_image_width and src_image_height assumes that the texels are tightly packed
    // in the buffer
    // Unspecified src_tex_width, src_tex_height, src_tex_depth is the same as the texture's dimensions
    FORCEINLINE void CopyTextureToBuffer (RHITexture * texture, RHIBuffer * buffer, size_t buffer_offset = 0,
                                    uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t layer_count = 1,
                                    uint32_t dst_tex_width = 0, uint32_t dst_tex_height = 0,
                                    int src_tex_x = 0, int src_tex_y = 0, int src_tex_z = 0,
                                    uint32_t src_tex_width = 0, uint32_t src_tex_height = 0, uint32_t src_tex_depth = 0) {
        AddCommand(AllocateCommand<RHICommandCopyTextureToBuffer>(
                texture, buffer, buffer_offset,
                mip_level, base_layer, layer_count,
                dst_tex_width, dst_tex_height,
                src_tex_x, src_tex_y, src_tex_z,
                src_tex_width, src_tex_height, src_tex_depth));
    }
    FORCEINLINE void CopyBuffer (RHIBufferSpan src, RHIBufferSpan dst) {
        AddCommand(AllocateCommand<RHICommandCopyBuffer>(src, dst));
    }

    FORCEINLINE void UpdateDrawState (const RHIDrawStateDesc & draw_state) {
        AddCommand(AllocateCommand<RHICommandUpdateDrawState>(draw_state));
    }

    FORCEINLINE void BeginRendering () {
        AddCommand(AllocateCommand<RHICommandBeginRendering>());
    }
    FORCEINLINE void EndRendering () {
        AddCommand(AllocateCommand<RHICommandEndRendering>());
    }

    FORCEINLINE void Draw (uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex = 0, uint32_t first_instance = 0) {
        AddCommand(AllocateCommand<RHICommandDraw>(vertex_count, instance_count, first_vertex, first_instance));
    }
    FORCEINLINE void DrawIndexed (RHIBufferSpan index_buffer, uint32_t index_count,
                                          uint32_t instance_count, uint32_t first_index, int base_vertex_index,
                                          uint32_t first_instance_index, RHIIndexType index_type) {
        AddCommand(AllocateCommand<RHICommandDrawIndexed>(index_buffer, index_count, instance_count, first_index, base_vertex_index, first_instance_index, index_type));
    }
    FORCEINLINE void DrawIndirect (RHIBufferSpan commands, uint32_t count = 1) {
        AddCommand(AllocateCommand<RHICommandDrawIndirect>(commands, count));
    }
    FORCEINLINE void DrawIndexedIndirect (RHIBufferSpan index_buffer, RHIBufferSpan commands, uint32_t count = 1, RHIIndexType type = RHIIndexType::kUint32) {
        AddCommand(AllocateCommand<RHICommandDrawIndexedIndirect>(index_buffer, commands, count, type));
    }
    FORCEINLINE void SetScissor (int x, int y, uint32_t width, uint32_t height) {
        AddCommand(AllocateCommand<RHICommandSetScissor>(x, y, width, height));
    }
    FORCEINLINE void SetViewport (float x, float y, float width, float height, float min_depth = 0.f, float max_depth = 1.f) {
        AddCommand(AllocateCommand<RHICommandSetViewport>(x, y, width, height, min_depth, max_depth));
    }
    FORCEINLINE void SetCullMode (RHICullModeType cull_mode) {
        AddCommand(AllocateCommand<RHICommandSetCullMode>(cull_mode));
    }

    FORCEINLINE void Dispatch (uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z) {
        AddCommand(AllocateCommand<RHICommandDispatch>(group_count_x, group_count_y, group_count_z));
    }
    FORCEINLINE void DispatchIndirect (RHIBuffer * dispatch_command_buffer, uint32_t offset) {
        AddCommand(AllocateCommand<RHICommandDispatchIndirect>(dispatch_command_buffer, offset));
    }
    // Create a single signature parameter table. This is a batch-of-1 shortcut around the batch
    // command below (mirrors BufferBarrier vs BufferBarriers). Signature unchanged for callers.
    FORCEINLINE void CreateSignatureParameterTable(
        uint32_t table_id,
        RHIPipelineRootSignature * root_signature,
        RHIBindPipelineParametersDesc desc) {
        auto * entry = Allocate<RHISignatureParameterTableCreateInfo[]>(1);
        entry[0] = { root_signature, desc };
        AddCommand(AllocateCommand<RHICommandCreateSignatureParameterTables>(table_id, 1, entry));
    }
    // Batch-create `count` signature parameter tables with contiguous ids
    // [base_table_id, base_table_id + count). `tables` points to frame-local storage the caller
    // allocated via the command buffer and must remain valid until the command executes.
    FORCEINLINE void CreateSignatureParameterTables(
        uint32_t base_table_id, uint32_t count, RHISignatureParameterTableCreateInfo * tables) {
        AddCommand(AllocateCommand<RHICommandCreateSignatureParameterTables>(base_table_id, count, tables));
    }
    FORCEINLINE void BindSignatureParameterTable(uint32_t table_id, RHIBindPointType point) {
        AddCommand(AllocateCommand<RHICommandBindSignatureParameterTable>(table_id, point));
    }

    // Push a block of push constant data to the pipeline currently bound on the given bind point.
    // BindPipeline must be called for that bind point before the dispatch/draw that consumes it.
    // @param data push constant bytes. A copy is made into frame-local memory, so the caller may pass a stack struct.
    // @param point which bind point (graphics/compute/ray tracing) the pipeline is bound on.
    // @param stages which shader stages receive the data. Narrow this to the stages that actually
    //               read the push constant (e.g. kCompute for a compute pipeline) to avoid pushing
    //               to unused stages. Must be a subset of the pipeline layout's push constant range.
    FORCEINLINE void PushConstants(std::span<std::byte> data, RHIBindPointType point,
                                   RHIShaderFrequencyFlags stages = RHIShaderFrequencyFlagBits::kAll) {
        if (data.empty()) return;
        auto * copy = static_cast<std::byte*>(AllocateRaw(data.size()));
        memcpy(copy, data.data(), data.size());
        AddCommand(AllocateCommand<RHICommandPushConstants>(
            std::span<std::byte>(copy, data.size()), point, stages));
    }

    FORCEINLINE void BindVertexBuffer (uint32_t binding, RHIBufferSpan buffer) {
        // TODO switch to batched binding (bind vertex buffers)
        AddCommand(AllocateCommand<RHICommandBindVertexBuffer>(binding, buffer));
    }

    FORCEINLINE void MemoryBarrier (
            RHIPipelineStageFlags src_stages = RHIPipelineStageFlagBits::kAll,
            RHIPipelineStageFlags dst_stages = RHIPipelineStageFlagBits::kAll,
            RHIGPUAccessFlags src_access = RHIGPUAccessFlagBits::kAll,
            RHIGPUAccessFlags dst_access = RHIGPUAccessFlagBits::kAll
    ) {
        AddCommand(AllocateCommand<RHICommandMemoryBarrier>(src_stages, dst_stages, src_access, dst_access));
    }

    FORCEINLINE void TextureBarrier (
            RHITexture * texture, RHITextureLayoutType layout,
            RHIPipelineStageFlags src_stages, RHIPipelineStageFlags dst_stages,
            RHIGPUAccessFlags src_access, RHIGPUAccessFlags dst_access
    ) {
        auto * layouts_ptr = Allocate<RHITextureLayoutType>();
        layouts_ptr[0] = layout;
        auto src_stages_ptr = Allocate<RHIPipelineStageFlags>();
        src_stages_ptr[0] = src_stages;
        auto dst_stages_ptr = Allocate<RHIPipelineStageFlags>();
        dst_stages_ptr[0] = dst_stages;
        auto src_access_ptr = Allocate<RHIGPUAccessFlags>();
        src_access_ptr[0] = src_access;
        auto dst_access_ptr = Allocate<RHIGPUAccessFlags>();
        dst_access_ptr[0] = dst_access;
        auto * texture_ptr = Allocate<RHITexture*>();
        texture_ptr[0] = texture;
        AddCommand(AllocateCommand<RHICommandTextureBarrier>(1, texture_ptr, layouts_ptr, src_stages_ptr, dst_stages_ptr, src_access_ptr, dst_access_ptr));
    }

    FORCEINLINE void TextureBarriers (
        uint32_t texture_count, RHITexture ** textures, RHITextureLayoutType * layouts,
        RHIPipelineStageFlags * src_stages, RHIPipelineStageFlags * dst_stages,
        RHIGPUAccessFlags * src_accesses, RHIGPUAccessFlags * dst_accesses
    ) {
        AddCommand(AllocateCommand<RHICommandTextureBarrier>(texture_count, textures, layouts, src_stages, dst_stages, src_accesses, dst_accesses));
    }

    FORCEINLINE void BufferBarrier (
            RHIBufferSpan buffer,
            RHIPipelineStageFlags src_stages, RHIPipelineStageFlags dst_stages,
            RHIGPUAccessFlags src_access, RHIGPUAccessFlags dst_access
    ) {
        auto * desc = Allocate<RHIBufferSpan[]>(1);
        desc[0] = buffer;
        auto src_stages_ptr = Allocate<RHIPipelineStageFlags>();
        src_stages_ptr[0] = src_stages;
        auto dst_stages_ptr = Allocate<RHIPipelineStageFlags>();
        dst_stages_ptr[0] = dst_stages;
        auto src_access_ptr = Allocate<RHIGPUAccessFlags>();
        src_access_ptr[0] = src_access;
        auto dst_access_ptr = Allocate<RHIGPUAccessFlags>();
        dst_access_ptr[0] = dst_access;
        AddCommand(AllocateCommand<RHICommandBufferBarrier>(1, desc, src_stages_ptr, dst_stages_ptr, src_access_ptr, dst_access_ptr));
    }

    FORCEINLINE void BufferBarriers (
            uint32_t buffer_count, RHIBufferSpan * buffers,
            RHIPipelineStageFlags * src_stages, RHIPipelineStageFlags * dst_stages,
            RHIGPUAccessFlags * src_accesses, RHIGPUAccessFlags * dst_accesses
    ) {
        AddCommand(AllocateCommand<RHICommandBufferBarrier>(buffer_count, buffers, src_stages, dst_stages, src_accesses, dst_accesses));
    }

    FORCEINLINE void Barriers (
            uint32_t texture_count, RHITexture ** textures, RHITextureLayoutType * layouts,
            RHIPipelineStageFlags * tex_src_stages, RHIPipelineStageFlags * tex_dst_stages,
            RHIGPUAccessFlags * tex_src_accesses, RHIGPUAccessFlags * tex_dst_accesses,
            uint32_t buffer_count, RHIBufferSpan * buffers,
            RHIPipelineStageFlags * buf_src_stages, RHIPipelineStageFlags * buf_dst_stages,
            RHIGPUAccessFlags * buf_src_accesses, RHIGPUAccessFlags * buf_dst_accesses
    ) {
        AddCommand(AllocateCommand<RHICommandBarriers>(
            texture_count, textures, layouts, tex_src_stages, tex_dst_stages, tex_src_accesses, tex_dst_accesses,
            buffer_count, buffers, buf_src_stages, buf_dst_stages, buf_src_accesses, buf_dst_accesses
        ));
    }

    FORCEINLINE void AccelerationStructureBarrier (
            RHIAccelerationStructure * acceleration_structure,
            RHIPipelineStageFlags src_stages, RHIPipelineStageFlags dst_stages,
            RHIGPUAccessFlags src_access, RHIGPUAccessFlags dst_access
    ) {
        auto * desc = Allocate<RHIAccelerationStructure*>();
        desc[0] = acceleration_structure;
        auto src_stages_ptr = Allocate<RHIPipelineStageFlags>();
        src_stages_ptr[0] = src_stages;
        auto dst_stages_ptr = Allocate<RHIPipelineStageFlags>();
        dst_stages_ptr[0] = dst_stages;
        auto src_access_ptr = Allocate<RHIGPUAccessFlags>();
        src_access_ptr[0] = src_access;
        auto dst_access_ptr = Allocate<RHIGPUAccessFlags>();
        dst_access_ptr[0] = dst_access;
        AddCommand(AllocateCommand<RHICommandAccelerationStructureBarrier>(1, desc, src_stages_ptr, dst_stages_ptr, src_access_ptr, dst_access_ptr));
    }

    FORCEINLINE void AccelerationStructureBarriers (
            uint32_t acceleration_structure_count, RHIAccelerationStructure ** acceleration_structures,
            RHIPipelineStageFlags * src_stages, RHIPipelineStageFlags * dst_stages,
            RHIGPUAccessFlags * src_accesses, RHIGPUAccessFlags * dst_accesses
    ) {
        AddCommand(AllocateCommand<RHICommandAccelerationStructureBarrier>(acceleration_structure_count, acceleration_structures, src_stages, dst_stages, src_accesses, dst_accesses));
    }

    FORCEINLINE void BindPipeline(RHIGraphicsPipeline * pipeline, RHIPipelineRootSignature * root_signature) {
        AddCommand(AllocateCommand<RHICommandBindGraphicsPipeline>(pipeline, root_signature));
    }
    FORCEINLINE void BindPipeline(RHIComputePipeline * pipeline, RHIPipelineRootSignature * root_signature) {
        AddCommand(AllocateCommand<RHICommandBindComputePipeline>(pipeline, root_signature));
    }
    FORCEINLINE void BindPipeline(RHIRayTracingPipeline * pipeline, RHIPipelineRootSignature * root_signature) {
        AddCommand(AllocateCommand<RHICommandBindRayTracingPipeline>(pipeline, root_signature));
    }

    // Ray tracing commands
    FORCEINLINE void BuildAccelerationStructure(
        const RHIAccelerationStructureBuildGeometryInfo& build_info,
        RHIBufferSpan scratch_buffer) {
        AddCommand(AllocateCommand<RHICommandBuildAccelerationStructure>(build_info, scratch_buffer));
    }

    FORCEINLINE void BindShaderBindingTable(
        RHIBufferSpan raygen_sbt,
        RHIBufferSpan miss_sbt = {},
        RHIBufferSpan hit_sbt = {},
        RHIBufferSpan callable_sbt = {}) {
        AddCommand(AllocateCommand<RHICommandBindShaderBindingTable>(raygen_sbt, miss_sbt, hit_sbt, callable_sbt));
    }

    FORCEINLINE void DispatchRays(uint32_t width, uint32_t height, uint32_t depth = 1) {
        AddCommand(AllocateCommand<RHICommandDispatchRays>(width, height, depth));
    }

    FORCEINLINE void DispatchRaysIndirect(RHIBufferSpan raygen, RHIBufferSpan miss, uint64_t miss_stride, RHIBufferSpan hit, uint64_t hit_stride, RHIBufferSpan indirect_buffer) {
        AddCommand(AllocateCommand<RHICommandDispatchRaysIndirect>(raygen, miss, miss_stride, hit, hit_stride, indirect_buffer));
    }

    FORCEINLINE void DispatchRaysIndirect2(RHIBufferSpan indirect_buffer) {
        AddCommand(AllocateCommand<RHICommandDispatchRaysIndirect2>(indirect_buffer));
    }

    FORCEINLINE void BeginDebugMarker(
        [[maybe_unused]] const char* marker_name,
        [[maybe_unused]] const std::array<float, 4>& color = {1.0f, 1.0f, 1.0f, 1.0f}) {
#if MI_ENABLE_RHI_OBJECT_NAMING
        auto len = strlen(marker_name);
        auto name_copy = Allocate<char[]>(len + 1);
        memcpy(name_copy, marker_name, len + 1);
        AddCommand(AllocateCommand<RHICommandDebugMarkerBegin>(name_copy, color));
#endif
    }

    FORCEINLINE void EndDebugMarker() {
#if MI_ENABLE_RHI_OBJECT_NAMING
        AddCommand(AllocateCommand<RHICommandDebugMarkerEnd>());
#endif
    }

    FORCEINLINE void InsertDebugMarker(
        [[maybe_unused]] const char* marker_name,
        [[maybe_unused]] const std::array<float, 4>& color = {1.0f, 1.0f, 1.0f, 1.0f}) {
#if MI_ENABLE_RHI_OBJECT_NAMING
        auto len = strlen(marker_name);
        auto name_copy = Allocate<char[]>(len + 1);
        memcpy(name_copy, marker_name, len + 1);
        AddCommand(AllocateCommand<RHICommandDebugMarkerInsert>(name_copy, color));
#endif
    }

    // Insert a GPU timestamp at the specified pipeline stages. Default is all commands.
    FORCEINLINE void InsertTimestamp(RHITimestamp* timestamp,
                                     RHIPipelineStageFlagBits stage = RHIPipelineStageFlagBits::kAll) {
        AddCommand(AllocateCommand<RHICommandInsertTimestamp>(timestamp, stage));
    }

};

// No other kinds of command queues are needed for now.

class RHIScopedDebugMarker {
public:
    RHIScopedDebugMarker(RHICommandQueueGraphics& queue, const char* marker_name,
                         const std::array<float, 4>& color = {1.0f, 1.0f, 1.0f, 1.0f})
            : queue_(queue) {
        queue_.BeginDebugMarker(marker_name, color);
    }

    ~RHIScopedDebugMarker() {
        queue_.EndDebugMarker();
    }

private:
    RHICommandQueueGraphics& queue_;
};

#ifdef RHI_ENABLE_DEBUG_MARKERS

#define RHI_BEGIN_DEBUG_MARKER(queue, name) queue.BeginDebugMarker(name)
#define RHI_END_DEBUG_MARKER(queue) queue.EndDebugMarker()
#define RHI_INSERT_DEBUG_MARKER(queue, name) queue.InsertDebugMarker(name)
#define RHI_SCOPED_DEBUG_MARKER(queue, name) RHIScopedDebugMarker _scoped_marker_##__LINE__(queue, name)
#define RHI_SCOPED_DEBUG_MARKER_COLOR(queue, name, color) RHIScopedDebugMarker _scoped_marker_##__LINE__(queue, name, color)

#else

#define RHI_BEGIN_DEBUG_MARKER(queue, name)
#define RHI_END_DEBUG_MARKER(queue)
#define RHI_INSERT_DEBUG_MARKER(queue, name)
#define RHI_SCOPED_DEBUG_MARKER(queue, name)
#define RHI_SCOPED_DEBUG_MARKER_COLOR(queue, name, color)

#endif

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_CMD_H
