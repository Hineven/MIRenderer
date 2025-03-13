/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_CMD_H
#define RDG_CMD_H

#include "rdg/rdg.h"
MI_NAMESPACE_BEGIN

// Helpers for dispatching shaders, etc.
// Used inside pass lambdas.
class RDGCommands {
public:
    // RDG Pass API (automatically spawn resource dependencies)
    static void Dispatch (RenderGraph & graph, TRef<RDGShader> compute_shader, int x = 1, int y = 1, int z = 1) ;
    static void DispatchIndirect (RenderGraph & graph, TRef<RDGShader> compute_shader, TRef<RDGBuffer> indirect_buffer) ;
    static void Draw (RenderGraph & graph, TRef<RDGShader> graphics_shader, int vertex_count, int instance_count = 1, int first_vertex = 0, int first_instance = 0) ;
    static void DrawIndexed (RenderGraph & graph, TRef<RDGShader> graphics_shader, int index_count, int instance_count = 1, int first_index = 0, int vertex_offset = 0, int first_instance = 0) ;
    static void DrawIndirect (RenderGraph & graph, TRef<RDGShader> graphics_shader, TRef<RDGBuffer> indirect_buffer) ;
    static void DrawIndexedIndirect (RenderGraph & graph, TRef<RDGShader> graphics_shader, TRef<RDGBuffer> indirect_buffer) ;
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
