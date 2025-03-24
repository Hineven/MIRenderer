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

// Helpers for dispatching shaders, etc.
// Used inside pass lambdas.
class RDGCommandHelper {
public:
    // RDG Pass API (automatically spawn resource dependencies)
    static void Dispatch (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
        const RDGShaderParamStructAndSizeInfo * info, void * params, int x = 1, int y = 1, int z = 1) ;
    template<typename T>
    FORCEINLINE static void Dispatch (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader, void * params,
        int x = 1, int y = 1, int z = 1) {
        Dispatch(queue, pass, compute_shader, T::GetShaderParamStructInfo(), params, x, y, z);
    }
    static void DispatchIndirect (RHICommandQueueGraphics & queue, RenderGraph & graph, TRef<RDGShader> compute_shader, TRef<RDGBuffer> indirect_buffer) ;
    static void Draw (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, void * params,
        int vertex_count, int instance_count = 1, int first_vertex = 0, int first_instance = 0) ;
    static void DrawIndexed (RenderGraph & graph, TRef<RDGShader> graphics_shader, int index_count, int instance_count = 1, int first_index = 0, int vertex_offset = 0, int first_instance = 0) ;
    static void DrawIndirect (RenderGraph & graph, TRef<RDGShader> graphics_shader, TRef<RDGBuffer> indirect_buffer) ;
    static void DrawIndexedIndirect (RenderGraph & graph, TRef<RDGShader> graphics_shader, TRef<RDGBuffer> indirect_buffer) ;
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
