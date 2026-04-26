/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_HELPER_H
#define RDG_HELPER_H

#include "rdg_builder.h"
#include "rdg/rdg.h"
#include "rdg/rdg_cmd.h"
MI_NAMESPACE_BEGIN

// Simple helpers for easily adding commonly used RDG passes. As well as invoking raw RHI commands.
class Helpers {
public:

    static void Clear(RenderGraphBuilder & builder, RDGBuffer * buffer, uint32_t value = 0, size_t offset = 0, size_t size = SIZE_MAX);

    static void Clear(RenderGraphBuilder & builder, RDGTexture * texture, glm::vec4 clear_value = {}, uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t num_layers = 1);

    static void CopyTexture(RenderGraphBuilder & builder, RDGTexture * src, RDGTexture * dst,
                         uint32_t src_mip_level = 0, uint32_t src_base_layer = 0, uint32_t src_layer_count = 1,
                         uint32_t dst_mip_level = 0, uint32_t dst_base_layer = 0, uint32_t dst_layer_count = 1);
    static void CopyBuffer(RenderGraphBuilder & builder, RDGBuffer * src, RDGBuffer * dst, size_t size = SIZE_MAX, size_t src_offset = 0, size_t dst_offset = 0);

    static TRef<RDGBuffer> SpawnDrawIndirectCommand (RenderGraphBuilder & builder, BufferPtrOrUint vertex_count, BufferPtrOrUint instance_count = 1, uint32_t first_vertex = 0, uint32_t first_instance = 0) ;

    // Spawn a pass that creates a dispatch indirect command with the specified number of thread groups (up divided by up_divisor).
    static TRef<RDGBuffer> SpawnDispatchIndirectCommand1D (RenderGraphBuilder & builder, RDGBuffer * count_buffer, uint32_t up_divisor = 1);

    // Spawn a pass that creates a trace rays indirect command for 1D ray tracing.
    // @param trace_rays_2 Whether to use DispatchRays2 (with a modified dispatch command structure and a newer graphics API)
    static TRef<RDGBuffer> SpawnTraceRaysIndirectCommand1D (RenderGraphBuilder & builder, RDGShader * ray_tracing_shader, RDGBuffer * count_buffer, bool trace_rays_2 = false) ;

    template<CShaderType T>
    FORCEINLINE static RDGPass * AddComputePass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, uint32_t x = 1, uint32_t y = 1, uint32_t z = 1, RDGPassFlags flags = {}) {
        if (!shader) return nullptr;
        mi_assert(shader->GetPipelineType() == RHIPipelineType::kCompute, "Only compute shaders are supported in DispatchComputePass.");
        return builder.AddPass<T>(flags, shader, params,
            [shader, params, x, y, z](RDGPass * pass, RHICommandQueueGraphics & queue) {
                auto tid = RDGCommandHelper::CreateParameterTable(queue, pass, shader, params);
                RDGCommandHelper::Dispatch(queue, shader, tid, x, y, z);
            }
        );
    }

    template<CShaderType T>
    FORCEINLINE static RDGPass * AddComputeIndirectPass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, RDGBuffer * indirect_buffer) {
        if (!shader || !indirect_buffer) return nullptr;
        mi_assert(shader->GetPipelineType() == RHIPipelineType::kCompute, "Only compute shaders are supported in DispatchIndirectComputePass.");
        return builder.AddPass<T>({}, shader, params,
            [shader, params, indirect_buffer](RDGPass * pass, RHICommandQueueGraphics & queue) {
                auto tid = RDGCommandHelper::CreateParameterTable(queue, pass, shader, params);
                RDGCommandHelper::DispatchIndirect(queue, shader, tid, indirect_buffer);
            }
        )->AddBuffer(indirect_buffer, RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }

    template<CShaderType T>
    FORCEINLINE static RDGPass * AddDrawIndirectPass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, RDGBuffer * indirect_buffer, uint32_t count = 1) {
        if (!shader || !indirect_buffer) return nullptr;
        mi_assert(shader->GetPipelineType() == RHIPipelineType::kGraphics, "Only graphics shaders are supported in DrawIndirectPass.");
        return builder.AddPass<T>({}, shader, params,
            [shader, params, indirect_buffer, draw_count=count](RDGPass* pass, RHICommandQueueGraphics &q){
                auto tid = RDGCommandHelper::CreateParameterTable(q, pass, shader, params);
                RDGCommandHelper::BeginGraphicsRender(q, shader, tid,
                    &T::GetShaderParamStructInfo()->render_pass_info_, params);
                q.DrawIndirect(indirect_buffer->GetRHI());
                RDGCommandHelper::EndGraphicsRender(q);
            }
        )->AddBuffer(indirect_buffer, RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }


    template<CShaderType T>
    FORCEINLINE static RDGPass * AddTraceRaysPass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, uint32_t x = 1, uint32_t y = 1, uint32_t z = 1, RDGPassFlags flags = {}) {
        if (!shader) return nullptr;
        mi_assert(shader->GetPipelineType() == RHIPipelineType::kRayTracing, "Only ray tracing shaders are supported in DispatchRayTracingPass.");
        return builder.AddPass<T>(flags, shader, params,
            [shader, params, x, y, z](RDGPass * pass, RHICommandQueueGraphics & queue) {
                auto tid = RDGCommandHelper::CreateParameterTable(queue, pass, shader, params);
                RDGCommandHelper::DispatchRays(queue, shader, tid, x, y, z);
            }
        );
    }

    template<CShaderType T>
    FORCEINLINE static RDGPass * AddTraceRaysIndirectPass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, RDGBuffer * indirect_buffer) {
        if (!shader || !indirect_buffer) return nullptr;
        mi_assert(shader->GetPipelineType() == RHIPipelineType::kRayTracing, "Only ray tracing shaders are supported in DispatchIndirectRayTracingPass.");
        return builder.AddPass<T>({}, shader, params,
            [shader, params, indirect_buffer](RDGPass * pass, RHICommandQueueGraphics & queue) {
                auto tid = RDGCommandHelper::CreateParameterTable(queue, pass, shader, params);
                RDGCommandHelper::DispatchRaysIndirect(queue, shader, tid, indirect_buffer);
            }
        )->AddBuffer(indirect_buffer, RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }


    // Enqueue upload commands to the RHI graphics command queue and place barriers.
    // If you want that happen immediately, launch a submit on the queue and wait idle.
    static void Upload_Async (RHIBufferSpan buffer, const void * data, size_t size) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    static void Upload_Async (RHICommandQueueGraphics & queue, RHIBufferSpan buffer, const void * data, size_t size) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    static void Upload_Async (RHICommandQueueGraphics & queue, RHITexture * texture, const void * data, size_t size, RHITextureLayoutType dst_layout, RHIGPUAccessFlags dst_access) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    template<CMemTrivial T>
    static void Upload_Async (RHICommandQueueGraphics & queue, RHIBuffer * buffer, size_t offset, const T & data) {
        Upload_Async(queue, RHIBufferSpan{buffer, offset, sizeof(T)}, &data, sizeof(T));
    }
    // Add a RDG pass to upload data to a buffer. Will not track buffer usage in RDG.
    static void UploadWithRDG_Unsafe (RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDG (RenderGraphBuilder & builder, RDGBuffer * buffer, const void * data, size_t size, size_t dst_offset = 0) ;
    // Add a RDG pass to copy data back to host memory.
    static void ReadbackWithRDG (RenderGraphBuilder & builder, RDGBuffer * buffer, size_t src_offset, RHIBufferSpan readback_buffer) ;
    // Add a RDG pass to copy data back to host memory. Will not track buffer usage in RDG.
    static void ReadbackWithRDG_Unsafe (RenderGraphBuilder & builder, RHIBufferSpan buffer, RHIBufferSpan readback_buffer) ;
    // Copy back data to host memory.
    static void Readback (RHICommandQueueGraphics & queue, RHIBufferSpan buffer, void * data) ;

    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDGUsingStagingBuffer (RenderGraphBuilder & builder, RHIBufferSpan buffer, RHIBufferSpan staging_buffer, const void * data, size_t size) ;

    // Clear a buffer asynchronously to a 4-byte clear value.
    static void Clear_Async (RHICommandQueueGraphics & queue, RHIBufferSpan buffer, uint32_t clear_value = 0) ;
};

MI_NAMESPACE_END

#endif //RDG_HELPER_H
