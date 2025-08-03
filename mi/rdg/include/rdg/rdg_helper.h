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
    static void Clear(RenderGraphBuilder & builder, RDGTexture * texture, glm::vec4 clear_value = {}, uint32_t mip_level = 0, uint32_t base_layer = 0, uint32_t num_layers = 1);

    // Spawn a pass that creates a dispatch indirect command with the specified number of thread groups.
    static TRef<RDGBuffer> SpawnDispatchIndirectCommand1D (RenderGraphBuilder & builder, RDGBuffer * count_buffer, uint32_t up_divisor = 1);

    // Spawn a pass that creates a trace rays indirect command for 1D ray tracing.
    static TRef<RDGBuffer> SpawnTraceRaysIndirectCommand1D (RenderGraphBuilder & builder, RDGShader * ray_tracing_shader, RDGBuffer * count_buffer) ;

    template<CShaderType T>
    FORCEINLINE static RDGPass * DispatchComputePass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, uint32_t x = 1, uint32_t y = 1, uint32_t z = 1, RDGPassFlags flags = {}) {
        if (!shader) return nullptr;
        return builder.AddPass<T>(flags, shader, params,
            [shader, params, x, y, z](RDGPass * pass, RHICommandQueueGraphics & queue) {
                RDGCommandHelper::Dispatch<T>(queue, pass, shader, params, x, y, z);
            }
        );
    }

    template<CShaderType T>
    FORCEINLINE static RDGPass * DispatchIndirectComputePass(RenderGraphBuilder & builder, T * shader, typename T::ShaderParameters * params, RDGBuffer * indirect_buffer) {
        if (!shader || !indirect_buffer) return nullptr;
        return builder.AddPass<T>({}, shader, params,
            [shader, params, indirect_buffer](RDGPass * pass, RHICommandQueueGraphics & queue) {
                RDGCommandHelper::DispatchIndirect<T>(queue, pass, shader, params, indirect_buffer);
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
