/*
 * Created: 2024/7/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_VK_CMD_EXEC_H
#define MIRENDERER_VK_CMD_EXEC_H

#include "rhi/rhi_cmd.h"
#include "../rhi_cmd_exec.h"
MI_NAMESPACE_BEGIN

class VulkanBuffer;
class VulkanGraphicsPipeline;
class VulkanComputePipeline;

class VulkanCommandExecutor : public RHICommandExecutor {
public:
    VulkanCommandExecutor() ;
    virtual ~VulkanCommandExecutor() override ;

    void RHICopyBuffer(RHICommandQueueBase * cmd, RHICommandCopyBuffer * copy_buffer) override ;
    void RHICopyTexture(RHICommandQueueBase * cmd, RHICommandCopyTexture * copy_texture) override ;
    void RHIDrawPrimitive(RHICommandQueueBase * cmd, RHICommandDrawPrimitive * draw_primitive) override ;
    void RHIDrawIndexedPrimitive(RHICommandQueueBase * cmd, RHICommandDrawIndexedPrimitive * draw_indexed_primitive) override ;
    void RHIDispatch(RHICommandQueueBase * cmd, RHICommandDispatch * dispatch) override ;
    void RHIBindGraphicsPipeline(RHICommandQueueBase * cmd, RHICommandBindGraphicsPipeline * bind_graphics_pipeline) override ;
    void RHIBindComputePipeline(RHICommandQueueBase * cmd, RHICommandBindComputePipeline * bind_compute_pipeline) override ;
    void RHIBindPipelineParameters(RHICommandQueueBase * cmd, RHICommandBindPipelineParameters * bind_pipeline_parameters) override ;
    void RHIBindVertexBuffer(RHICommandQueueBase * cmd, RHICommandBindVertexBuffer * bind_vertex_buffer) override ;
protected:

    typedef std::span<vk::WriteDescriptorSet> DescriptorWrites;

    void FlushBindPointDescriptorWrites (RHICommandQueueBase *, RHIBindPointType) ;

    struct CommandQueueState {
        vk::CommandBuffer cmd {};
        // Can bind up to 8 vertex buffers
        RHIBufferSpan bound_vertex_buffers[8] {};
        struct BindPoints {
            // Bindless table buffer
            VulkanBuffer * bindless_table_buffer {};
            // Bindless table allocation offset
            uint32_t bindless_table_top;

            // Bound private descriptor set
            vk::DescriptorSet bound_private_set{};
            RHIPipeline * bound_pipeline{};
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

                // Clear table and launch descriptor writes
                // Return the descriptor writes and bindless table contents
                DescriptorWrites GenerateDescriptorWritesAndPlaceBarriers (
                        RHICommandQueueBase * cmd, vk::Device device,
                        vk::DescriptorSet descriptor_set, std::span<std::uint32_t> btb_data
                );
                // Merge incoming table
                void Merge (const RHIBindPipelineParametersDesc * desc);
            } parameter_table;


        } points[(uint32_t)RHIBindPointType::kMax];
        vk::DescriptorPool descriptor_pool {};

        CommandQueueState();
        ~CommandQueueState();

    } state_[(uint32_t)RHICommandQueueType::kMax];
};

MI_NAMESPACE_END
#endif //MIRENDERER_VK_CMD_EXEC_H
