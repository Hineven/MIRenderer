/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_CMD_H
#define RDG_CMD_H

#include "rdg/rdg.h"
MI_NAMESPACE_BEGIN

struct RDGShaderParamStructAndSizeInfo;

template<typename T>
concept CShaderType = std::is_base_of<RDGShader, std::remove_cvref_t<T>>::value;

// Helpers for dispatching shaders, etc.
// NOTE: Use them inside pass lambdas.
class RDGCommandHelper {
public:

    // Convenience function to upload shader parameters
    static std::optional<RHIBindPipelineParametersDesc> SetupShaderParams(
        RDGPass * pass, RDGShader * shader, RHICommandQueueGraphics & queue,
        const RDGShaderParamStructAndSizeInfo * base_info, const void * params) ;
    template <CShaderType T>
    FORCEINLINE static RHIBindPipelineParametersDesc SetupShaderParams(
        RDGPass * pass, T * shader, RHICommandQueueGraphics & queue, const typename T::ShaderParameters * params) {
        return SetupShaderParams(pass, shader, queue, T::GetShaderParamStructInfo(), params);
    }

    // Bind a graphics shader, setting up required shader parameters and bindings for draw commands.
    // @param manual_vbuffer If true, the function will not automatically bind the vertex buffer specified in shader params.
    static bool BindGraphicsShader (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params, bool manual_vbuffer = false) ;

    // Bind a compute shader, setting up required shader parameters and bindings for dispatch commands.
    static bool BindComputeShader (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params) ;

    template<CShaderType T>
    FORCEINLINE static bool BindGraphicsShader (
        RHICommandQueueGraphics & queue, RDGPass * pass, T * graphics_shader, const typename T::ShaderParameters * params,
        bool manual_vbuffer = false
    ) {
        return BindGraphicsShader(queue, pass, graphics_shader, T::GetShaderParamStructInfo(), params, manual_vbuffer);
    }

    // RDG Pass API (automatically spawn resource dependencies)
    static void Dispatch (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
        const RDGShaderParamStructAndSizeInfo * info, const void * params, uint32_t x = 1, uint32_t y = 1, uint32_t z = 1) ;
    template<CShaderType T>
    FORCEINLINE static void Dispatch (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader, const typename T::ShaderParameters * params,
        uint32_t x = 1, uint32_t y = 1, uint32_t z = 1) {
        Dispatch(queue, pass, compute_shader, T::GetShaderParamStructInfo(), params, x, y, z);
    }

    static void DispatchIndirect (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params, RDGBuffer * indirect_buffer, uint32_t offset = 0) ;

    template<CShaderType T>
    FORCEINLINE static void DispatchIndirect (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader, const typename T::ShaderParameters * params, RDGBuffer * indirect_buffer) {
        DispatchIndirect(queue, pass, compute_shader, T::GetShaderParamStructInfo(), params, indirect_buffer);
    }

    static void Draw (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader,
        const RDGShaderParamStructAndSizeInfo * info, const void * params,
        int vertex_count, int instance_count = 1, int first_vertex = 0, int first_instance = 0) ;
    template<CShaderType T>
    FORCEINLINE static void Draw (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, const typename T::ShaderParameters * params,
        int vertex_count, int instance_count = 1, int first_vertex = 0, int first_instance = 0) {
        Draw(queue, pass, graphics_shader, T::GetShaderParamStructInfo(), params, vertex_count, instance_count, first_vertex, first_instance);
    }

    static void DrawIndexed (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, int index_count, int instance_count = 1, int first_index = 0, int vertex_offset = 0, int first_instance = 0) ;
    static void DrawIndirect (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, RDGBuffer * indirect_buffer) ;
    static void DrawIndexedIndirect (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, RDGBuffer * indirect_buffer) ;
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
