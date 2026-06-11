/*
 * Created: 2024/7/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VK_CMD_EXEC_H
#define MI_VK_CMD_EXEC_H

#include "rhi/rhi_cmd.h"
#include "rhi/rhi_cmd_exec.h"
#include "rhi/rhi_pipeline.h"
#include "vk_constants.h"
#include "vk_texture.h"
#include <array>

MI_NAMESPACE_BEGIN

class VulkanBuffer;
class VulkanGraphicsPipeline;
class VulkanComputePipeline;
class VulkanRootSignature;

// Command executor translating recoreded commands into Vulkan API calls
// Public functions are only invoked by the RHI thread
class VulkanCommandExecutor : public RHICommandExecutorInterface {
public:
    VulkanCommandExecutor() ;
    virtual ~VulkanCommandExecutor() override ;

    void RHIClearTexture(RHICommandQueueBase * cmd, RHICommandClearTexture * clear_texture) override ;
    void RHIClearBuffer(RHICommandQueueBase * cmd, RHICommandClearBuffer * clear_buffer) override ;
    void RHICopyBuffer(RHICommandQueueBase * cmd, RHICommandCopyBuffer * copy_buffer) override ;
    void RHICopyBufferToTexture(RHICommandQueueBase * cmd, RHICommandCopyBufferToTexture * copy_buffer_to_texture) override ;
    void RHICopyTextureToBuffer(RHICommandQueueBase * cmd, RHICommandCopyTextureToBuffer * copy_texture_to_buffer) override ;
    void RHICopyTexture(RHICommandQueueBase * cmd, RHICommandCopyTexture * copy_texture) override ;
    void RHIBlitTexture(RHICommandQueueBase *buffer, RHICommandBlitTexture *cmd) override;
    void RHIBeginRendering (RHICommandQueueBase * cmd, RHICommandBeginRendering * begin_rendering) override ;
    void RHIEndRendering (RHICommandQueueBase * cmd, RHICommandEndRendering * end_rendering) override ;
    void RHIDraw(RHICommandQueueBase * cmd, RHICommandDraw * draw_primitive) override ;
    void RHIDrawIndexed(RHICommandQueueBase * cmd, RHICommandDrawIndexed * draw_indexed_primitive) override ;
    void RHIDrawIndirect (RHICommandQueueBase * cmd, RHICommandDrawIndirect * draw_indirect) override ;
    void RHIDrawIndexedIndirect (RHICommandQueueBase * cmd, RHICommandDrawIndexedIndirect * draw_indexed_indirect) override ;
    void RHIDispatch(RHICommandQueueBase * cmd, RHICommandDispatch * dispatch) override ;
    void RHIDispatchIndirect(RHICommandQueueBase * cmd, RHICommandDispatchIndirect * dispatch_indirect) override ;
    void RHIBindGraphicsPipeline(RHICommandQueueBase * cmd, RHICommandBindGraphicsPipeline * bind_graphics_pipeline) override ;
    void RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetViewport *set_viewport) override;
    void RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetScissor *set_scissor) override;
    void RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetCullMode *set_cull_mode) override;
    void RHIUpdateDrawState(RHICommandQueueBase * cmd, RHICommandUpdateDrawState * update_draw_state) override ;
    void RHIBindComputePipeline(RHICommandQueueBase * cmd, RHICommandBindComputePipeline * bind_compute_pipeline) override ;
    void RHICreateSignatureParameterTable(RHICommandQueueBase * cmd, RHICommandCreateSignatureParameterTable * create_table) override ;
    void RHIBindSignatureParameterTable(RHICommandQueueBase * cmd, RHICommandBindSignatureParameterTable * bind_table) override ;
    void RHIBindVertexBuffer(RHICommandQueueBase * cmd, RHICommandBindVertexBuffer * bind_vertex_buffer) override ;
    void RHIPushConstants(RHICommandQueueBase * cmd, RHICommandPushConstants * push_constants) override ;
    void RHIMemoryBarrier (RHICommandQueueBase * buffer, RHICommandMemoryBarrier * cmd) override ;
    void RHITextureBarrier(RHICommandQueueBase * cmd, RHICommandTextureBarrier * barrier) override ;
    void RHIBufferBarriers(RHICommandQueueBase * cmd, RHICommandBufferBarrier * barrier) override ;
    void RHICombinedBarriers(RHICommandQueueBase * cmd, RHICommandBarriers * barrier) override ;
    void RHIDebugMarkerBegin(RHICommandQueueBase *buffer, RHICommandDebugMarkerBegin *cmd) override;
    void RHIDebugMarkerEnd(RHICommandQueueBase *buffer, RHICommandDebugMarkerEnd *cmd) override;
    void RHIDebugMarkerInsert(RHICommandQueueBase *buffer, RHICommandDebugMarkerInsert *cmd) override;
    void RHIInsertTimestamp (RHICommandQueueBase * buffer, RHICommandInsertTimestamp * cmd) override;

    // Ray tracing commands
    void RHIBuildAccelerationStructure(RHICommandQueueBase *cmd, RHICommandBuildAccelerationStructure *build_acceleration_structure) override;
    void RHIBindRayTracingPipeline(RHICommandQueueBase *cmd, RHICommandBindRayTracingPipeline *bind_ray_tracing_pipeline) override;
    void RHIBindShaderBindingTable(RHICommandQueueBase *cmd, RHICommandBindShaderBindingTable *bind_shader_binding_table) override;
    void RHIDispatchRays(RHICommandQueueBase *cmd, RHICommandDispatchRays *dispatch_rays) override;
    void RHIDispatchRaysIndirect(RHICommandQueueBase *cmd, RHICommandDispatchRaysIndirect *dispatch_rays_indirect) override;
    void RHIDispatchRaysIndirect2(RHICommandQueueBase *cmd, RHICommandDispatchRaysIndirect2 *dispatch_rays_indirect) override;
    void RHIAcclerationStructureBarriers(RHICommandQueueBase *cmd, RHICommandAccelerationStructureBarrier *barrier) override;

    void RHIFrameEnd(RHICommandQueueBase * cmd, RHISyncPoint * sync) override ;

    void RHISubmitCommandBuffer (RHICommandQueueBase * buffer, RHISyncPoint * sync,
        const std::string & submit_prefix, bool release_resources) override ;

    void * GetCurrentNativeCommandBuffer (RHICommandQueueType type) override ;
protected:

    void CheckDrawReadyness (RHICommandQueueBase * ) ;

    void FlushBindPointState (RHICommandQueueBase *, RHIBindPointType, vk::ShaderStageFlags use_shaders) ;

    void Initialize_RHIThread () ;
    void Destroy_RHIThread () ;

    struct CommandQueueState {
        // Each command queue has its own command pool and 1 single command buffer recording
        vk::CommandPool cmd_pool {};
        vk::CommandBuffer cmd {};
        // Keep track of allocated command buffers to free later. Used when RESET_COMMAND_POOL is false
        std::vector<vk::CommandBuffer> cmd_buffers_to_free {};
        bool cmd_recording_started {};

        // Internal states
        // Can bind up to 8 vertex buffers
        RHIBufferSpan bound_vertex_buffers[8] {};
        RHIBufferSpan bound_index_buffer {};
        RHIIndexType  bound_index_type {RHIIndexType::kMax};
        // Kept draw state.
        RHIDrawStateDesc draw_state_ {};
        vk::Rect2D GetScissorRect ();
        vk::Viewport GetViewport ();
        void InstallDrawState (vk::CommandBuffer cmdb);
        void BindIndexBuffer (RHIBufferSpan span, RHIIndexType type) ;
        // Ray tracing shader binding table regions (cached for dispatch rays)
        vk::StridedDeviceAddressRegionKHR raygen_sbt {};
        vk::StridedDeviceAddressRegionKHR miss_sbt {};
        vk::StridedDeviceAddressRegionKHR hit_sbt {};
        vk::StridedDeviceAddressRegionKHR callable_sbt {};
        // Reset the above states. should be called after submitting a command buffer.
        void ResetStates();
        // Reset the command buffer, and reset internal states.
        // Should be called after submitting a command buffer.
        void Init (RHICommandQueueType type) ;
        void Destroy () ;
        // Clear the command buffer and descriptor sets.
        // @param return_resources_to_system: If true, the resources allocated by the command buffer and descriptor sets
        void Clear (bool return_resources_to_system) ;

        // Keep states of each bind point
        struct BindPoint {
            RHIBindPointType bind_point_type {};

            bool bound_pipeline_dirty {false};
            bool bound_descriptor_dirty {false};
            RHIPipeline * bound_pipeline {};
            vk::DescriptorSet bound_private_descriptor_set {};
            template<typename T>
            inline T * As() {
                return static_cast<T *>(bound_pipeline);
            }

            void Destroy ();
        } points[(uint32_t)RHIBindPointType::kMax];


        vk::DescriptorPool descriptor_pool {};
        std::vector<vk::DescriptorSet> allocated_descriptor_sets {};
        std::unordered_map<uint32_t, vk::DescriptorSet> slot_table_;

        // RHI thread only allocator for temporaries.
        TOneTimeLinearAllocator<> allocator {};

        FORCEINLINE void * Allocate(size_t size) {
            return allocator.Allocate(size);
        }
        template<CMemTrivial T>
        FORCEINLINE T * Allocate() {
            auto ptr = static_cast<T *>(Allocate(sizeof(T)));
            new (ptr) T();
            return ptr;
        }
        template<CAOUB T>
        FORCEINLINE std::remove_all_extents_t<T> * Allocate(size_t count) {
            using TElem = std::remove_all_extents_t<T>;
            auto ptr = static_cast<TElem *>(Allocate(sizeof(TElem) * count));
            new (ptr) TElem[count];
            return ptr;
        }


        void BeginCmd ();
        bool CloseCmd ();

        void SetupDefaultDynamicStates () const;

        // For debugging purposes only
#if MI_ENABLE_RHI_OBJECT_NAMING
        std::stack<std::string> debug_marker_stack;
        std::string last_inserted_debug_marker;
#endif
        FORCEINLINE void PushDebugMarker ([[maybe_unused]] const std::string & name) {
#if MI_ENABLE_RHI_OBJECT_NAMING
            debug_marker_stack.push(name);
#endif
        }
        FORCEINLINE void PopDebugMarker () {
#if MI_ENABLE_RHI_OBJECT_NAMING
            if (!debug_marker_stack.empty()) {
                debug_marker_stack.pop();
            } else {
                mi_assert(false, "Potential mismatch between PushDebugMarker and PopDebugMarker.");
            }
#endif
        }

        FORCEINLINE void CheckDebugMarkerStack () {
#if MI_ENABLE_RHI_OBJECT_NAMING
            if (!debug_marker_stack.empty()) {
                mi_assert(false, "Potential mismatch between PushDebugMarker and PopDebugMarker.");
            }
#endif
        }
    };

    struct {
        CommandQueueState states[2];
        int state_index {};
        inline CommandQueueState & Current(bool begin_cmd = true) {
            if (begin_cmd) {
                states[state_index].BeginCmd();
            }
            return states[state_index];
        }
    } state_chains_[(uint32_t)RHICommandQueueType::kMax];
};

MI_NAMESPACE_END
#endif //MI_VK_CMD_EXEC_H
