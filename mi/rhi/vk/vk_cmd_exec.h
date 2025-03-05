/*
 * Created: 2024/7/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VK_CMD_EXEC_H
#define MI_VK_CMD_EXEC_H

#include "rhi/rhi_cmd.h"
#include "../rhi_cmd_exec.h"
#include "rhi/rhi_pipeline.h"
#include "vk_constants.h"
#include "vk_texture.h"

MI_NAMESPACE_BEGIN

class VulkanBuffer;
class VulkanGraphicsPipeline;
class VulkanComputePipeline;

// Command executor translating recoreded commands into Vulkan API calls
// Public functions are only invoked by the RHI thread
class VulkanCommandExecutor : public RHICommandExecutorInterface {
public:
    VulkanCommandExecutor() ;
    virtual ~VulkanCommandExecutor() override ;

    void RHIClearTexture(RHICommandQueueBase * cmd, RHICommandClearTexture * clear_texture) override ;
    void RHICopyBuffer(RHICommandQueueBase * cmd, RHICommandCopyBuffer * copy_buffer) override ;
    void RHICopyBufferToTexture(RHICommandQueueBase * cmd, RHICommandCopyBufferToTexture * copy_buffer_to_texture) override ;
    void RHICopyTextureToBuffer(RHICommandQueueBase * cmd, RHICommandCopyTextureToBuffer * copy_texture_to_buffer) override ;
    void RHICopyTexture(RHICommandQueueBase * cmd, RHICommandCopyTexture * copy_texture) override ;
    void RHIBeginRendering (RHICommandQueueBase * cmd, RHICommandBeginRendering * begin_rendering) override ;
    void RHIEndRendering (RHICommandQueueBase * cmd, RHICommandEndRendering * end_rendering) override ;
    void RHIDrawPrimitive(RHICommandQueueBase * cmd, RHICommandDrawPrimitive * draw_primitive) override ;
    void RHIDrawIndexedPrimitive(RHICommandQueueBase * cmd, RHICommandDrawIndexedPrimitive * draw_indexed_primitive) override ;
    void RHIDispatch(RHICommandQueueBase * cmd, RHICommandDispatch * dispatch) override ;
    void RHIBindGraphicsPipeline(RHICommandQueueBase * cmd, RHICommandBindGraphicsPipeline * bind_graphics_pipeline) override ;
    void RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetViewport *set_viewport) override;
    void RHIUpdateDrawState(RHICommandQueueBase *cmd, RHICommandSetScissor *set_scissor) override;
    void RHIUpdateDrawState(RHICommandQueueBase * cmd, RHICommandUpdateDrawState * update_draw_state) override ;
    void RHIBindComputePipeline(RHICommandQueueBase * cmd, RHICommandBindComputePipeline * bind_compute_pipeline) override ;
    void RHIBindPipelineParameters(RHICommandQueueBase * cmd, RHICommandBindPipelineParameters * bind_pipeline_parameters) override ;
    void RHIBindVertexBuffer(RHICommandQueueBase * cmd, RHICommandBindVertexBuffer * bind_vertex_buffer) override ;
    void RHITextureBarrier(RHICommandQueueBase * cmd, RHICommandTextureBarrier * barrier) override ;
    void RHIBufferBarrier(RHICommandQueueBase * cmd, RHICommandBufferBarrier * barrier) override ;
    void RHIFrameEnd(RHICommandQueueBase * cmd, RHICommandFrameEnd * frame_end) override ;

    void RHISubmitCommandBuffer (RHICommandQueueBase * buffer, RHISyncPoint * sync, bool release_resources) override ;
protected:


    void FlushBindPointState (RHICommandQueueBase *, RHIBindPointType, vk::ShaderStageFlags use_shaders) ;

    void Initialize_RHIThread () ;
    void Destroy_RHIThread () ;

    typedef std::span<vk::WriteDescriptorSet> DescriptorWrites;

    struct CommandQueueState {
        // Each command queue has its own command pool and 1 single command buffer recording
        vk::CommandPool cmd_pool {};
        vk::CommandBuffer cmd {};
        bool cmd_recording_started {};
        // Can bind up to 8 vertex buffers
        RHIBufferSpan bound_vertex_buffers[8] {};

        // Kept draw state.
        RHIDrawDesc draw_state_ {};
        vk::Rect2D GetScissorRect ();
        vk::Viewport GetViewport ();
        void InstallDrawState (vk::CommandBuffer cmdb);

        // Keep states of each bind point
        struct BindPoints {
            // Bindless table buffer
            vk::Buffer bindless_table_buffer {};
            vma::Allocation bindless_table_buffer_allocation {};
            // Bindless table allocation offset (number of slots allocated)
            uint32_t bindless_table_top;
            // Mapped pointer for btb
            std::uint32_t * bindless_table_buffer_mapped {};

            // Dirty (pipeline & descriptor set)
            bool bound_pipeline_dirty {false};
            // Store a pointer to the pipeline should be bound to when dispatching commands
            RHIPipeline * bound_pipeline {};
            // Dirty (descriptors)
            bool bound_descriptor_dirty {false};
            // Bound private descriptor set (allocated from the descriptor pool)
            vk::DescriptorSet bound_private_descriptor_set {};
            template<typename T>
            inline T * As() {
                return static_cast<T *>(bound_pipeline);
            }
            struct ParameterTable {
                std::vector<RHIPipelineParameterBufferDesc> uniforms;
                std::vector<RHIPipelineParameterBufferDesc> storages;
                std::vector<RHIPipelineParameterTextureDesc> uavs;
                std::vector<RHIPipelineParameterTextureDesc> srvs;
                std::vector<RHIPipelineParameterResourceDesc> samplers;
                std::vector<RHIPipelineParameterResourceDesc> acceleration_structures;
                std::vector<RHIPipelineBindlessResourceDesc> bindless_resources;
                std::span<const std::byte> push_constants;
                // Merge incoming table, return dirty bit (if the merge has changed the state)
                bool Merge (const RHIBindPipelineParametersDesc * desc);
            } parameter_table;

            // Install bind point states, Clear parameter table and launch descriptor writes.
            // Note: Only image layout transition barriers are placed automatically.
            // You need to explicitly place memory / execution barriers.
            // Note(2): RDG handles these barriers.
            // @param use_stages: The stages that currently tabled resources will be used in.
            // @return: The descriptor writes and btb_data generated by this function.
            DescriptorWrites InstallShaderDescriptors (
                    RHICommandQueueBase * cmd, vk::Device device,
                    vk::DescriptorSet descriptor_set, std::span<std::uint32_t> btb_data,
                    vk::CommandBuffer cmdb
            );
        } points[(uint32_t)RHIBindPointType::kMax];


        vk::DescriptorPool descriptor_pool {};

        // Should be initialize & destroyed on the RHI thread only
        void Init (RHICommandQueueType type) ;
        void Destroy () ;
        // Clear the command buffer and descriptor sets.
        // @param return_resources_to_system: If true, the resources allocated by the command buffer and descriptor sets
        void Clear (bool return_resources_to_system) ;

        void BeginCmd ();
        bool CloseCmd ();

        void SetupDefaultDynamicStates ();
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
