/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_CMD_H
#define RDG_CMD_H

#include "rdg/rdg.h"
MI_NAMESPACE_BEGIN

struct RDGShaderSignatureParamInfo;
struct RDGShaderRenderPassInfo;

template<typename T>
concept CShaderType = std::is_base_of<RDGShader, std::remove_cvref_t<T>>::value;

class RDGCommandHelper {
public:

    // Allocate a parameter table id (unique). 
    static uint32_t AllocateParameterTableId();

    static uint32_t CreateParameterTable(
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * shader,
        const RDGShaderSignatureParamInfo * info, const void * params,
        bool populate_all = true);

    template <CShaderType T>
    FORCEINLINE static uint32_t CreateParameterTable(
        RHICommandQueueGraphics & queue, RDGPass * pass, T * shader,
        const typename T::ShaderParameters * params,
        bool populate_all = true) {
        return CreateParameterTable(queue, pass, shader,
            static_cast<const RDGShaderSignatureParamInfo*>(T::GetShaderParamStructInfo()), params,
            populate_all);
    }

    static void Dispatch (RHICommandQueueGraphics & queue, RDGShader * compute_shader,
        uint32_t table_id, uint32_t x = 1, uint32_t y = 1, uint32_t z = 1) ;

    static void DispatchIndirect (RHICommandQueueGraphics & queue, RDGShader * compute_shader,
        uint32_t table_id, RDGBuffer * indirect_buffer, uint32_t offset = 0) ;

    static void Draw (RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
        uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
        const void * params, int vertex_count, int instance_count = 1,
        int first_vertex = 0, int first_instance = 0) ;

    static void DispatchRays (RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
        uint32_t table_id, uint32_t width, uint32_t height, uint32_t depth = 1) ;

    static void DispatchRaysIndirect (
        RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
        uint32_t table_id, RDGBuffer * indirect_buffer) ;

    static void BeginGraphicsRender (RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
        uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
        const void * params) ;

    static void EndGraphicsRender (RHICommandQueueGraphics & queue) ;
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
