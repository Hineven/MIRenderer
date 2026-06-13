/*
 * Created: 2025/3/12
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_CMD_H
#define RDG_CMD_H

#include <span>
#include <cstddef>
#include "rdg/rdg.h"
MI_NAMESPACE_BEGIN

struct RDGShaderSignatureParamInfo;
struct RDGShaderRenderPassInfo;

template<typename T>
concept CShaderType = std::is_base_of<RDGShader, std::remove_cvref_t<T>>::value;

class RDGPassParameterTable;
class RenderGraph;

class RDGCommandHelper {
public:

    // Allocate a parameter table id (unique).
    static uint32_t AllocateParameterTableId();
    // Reserve a contiguous range of `count` parameter table ids and return the base.
    // The allocated ids are [base, base+count-1]. Used for batch table creation.
    static uint32_t AllocateParameterTableIds(uint32_t count);

    // Build a pipeline parameter descriptor from a shared parameter table.
    static std::optional<RHIBindPipelineParametersDesc> BuildParameterDesc(
        RHICommandQueueGraphics & queue,
        const RDGPassParameterTable & table,
        RenderGraph * graph);

    static void Dispatch (RHICommandQueueGraphics & queue, RDGShader * compute_shader,
        uint32_t table_id, uint32_t x = 1, uint32_t y = 1, uint32_t z = 1,
        std::span<std::byte> push_constants = {}) ;

    static void DispatchIndirect (RHICommandQueueGraphics & queue, RDGShader * compute_shader,
        uint32_t table_id, RDGBuffer * indirect_buffer, uint32_t offset = 0,
        std::span<std::byte> push_constants = {}) ;

    static void Draw (RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
        uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
        const void * params, int vertex_count, int instance_count = 1,
        int first_vertex = 0, int first_instance = 0,
        std::span<std::byte> push_constants = {}) ;

    static void DispatchRays (RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
        uint32_t table_id, uint32_t width, uint32_t height, uint32_t depth = 1,
        std::span<std::byte> push_constants = {}) ;

    static void DispatchRaysIndirect (
        RHICommandQueueGraphics & queue, RDGShader * ray_tracing_shader,
        uint32_t table_id, RDGBuffer * indirect_buffer,
        std::span<std::byte> push_constants = {}) ;

    static void BeginGraphicsRender (RHICommandQueueGraphics & queue, RDGShader * graphics_shader,
        uint32_t table_id, const RDGShaderRenderPassInfo * render_pass_info,
        const void * params, std::span<std::byte> push_constants = {}) ;

    static void EndGraphicsRender (RHICommandQueueGraphics & queue) ;

private:
    // Stage mask passed to vkCmdPushConstants. Always kAll: the (shared) pipeline layout declares its
    // push-constant range with VK_SHADER_STAGE_ALL, and VUID-vkCmdPushConstants-offset-01796 requires
    // the update's stageFlags to be a superset of the range's — so eAll range => eAll update. The
    // shader argument is retained for symmetry with the dispatch helpers but currently unused.
    static RHIShaderFrequencyFlags PushConstantStagesFor (const RDGShader * shader) ;
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
