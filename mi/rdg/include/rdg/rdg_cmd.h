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
// Used inside pass lambdas.
class RDGCommandHelper {
public:

    // Convenience function to upload shader parameters
    static std::optional<RHIBindPipelineParametersDesc> UploadShaderParams(
        RDGPass * pass, RDGShader * shader, RHICommandQueueGraphics & queue,
        const RDGShaderParamStructAndSizeInfo * base_info, const void * params) ;
    template <CShaderType T>
    FORCEINLINE static RHIBindPipelineParametersDesc UploadShaderParams(
        RDGPass * pass, T * shader, RHICommandQueueGraphics & queue, const void * params) {
        return UploadShaderParams(pass, shader, queue, T::GetShaderParamStructInfo(), params);
    }

    static bool BindGraphicsShader (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params, bool manual_vbuffer = false) ;

    template<CShaderType T>
    FORCEINLINE static bool BindGraphicsShader (
        RHICommandQueueGraphics & queue, RDGPass * pass, T * graphics_shader, const void * params,
        bool manual_vbuffer = false
    ) {
        return BindGraphicsShader(queue, pass, graphics_shader, T::GetShaderParamStructInfo(), params, manual_vbuffer);
    }

    // RDG Pass API (automatically spawn resource dependencies)
    static void Dispatch (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
        const RDGShaderParamStructAndSizeInfo * info, const void * params, int x = 1, int y = 1, int z = 1) ;
    template<CShaderType T>
    FORCEINLINE static void Dispatch (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader, const void * params,
        int x = 1, int y = 1, int z = 1) {
        Dispatch(queue, pass, compute_shader, T::GetShaderParamStructInfo(), params, x, y, z);
    }

    static void DispatchIndirect (RHICommandQueueGraphics & queue, RenderGraph & graph, TRef<RDGShader> compute_shader, TRef<RDGBuffer> indirect_buffer) ;

    static void Draw (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader,
        const RDGShaderParamStructAndSizeInfo * info, const void * params,
        int vertex_count, int instance_count = 1, int first_vertex = 0, int first_instance = 0) ;
    template<CShaderType T>
    FORCEINLINE static void Draw (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, const void * params,
        int vertex_count, int instance_count = 1, int first_vertex = 0, int first_instance = 0) {
        Draw(queue, pass, graphics_shader, T::GetShaderParamStructInfo(), params, vertex_count, instance_count, first_vertex, first_instance);
    }

    static void DrawIndexed (RenderGraph & graph, TRef<RDGShader> graphics_shader, int index_count, int instance_count = 1, int first_index = 0, int vertex_offset = 0, int first_instance = 0) ;
    static void DrawIndirect (RenderGraph & graph, TRef<RDGShader> graphics_shader, TRef<RDGBuffer> indirect_buffer) ;
    static void DrawIndexedIndirect (RenderGraph & graph, TRef<RDGShader> graphics_shader, TRef<RDGBuffer> indirect_buffer) ;
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
