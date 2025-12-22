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

// Shader context holder, used to ensure proper binding and unbinding of shader parameters.
// Correct usage: if(auto ctx = RDGCommandHelper::BindXXXShader(...)) { ...RHI calls... }
// Incorrect usage: if(RDGCommandHelper::BindXXXShader(...)) { ...RHI calls... }
// The context object must be kept alive during the RHI calls to ensure proper resource management.
struct RDGShaderContext {
    friend class RDGCommandHelper;
    FORCEINLINE RDGShaderContext (RHICommandQueueGraphics & queue, RHIBindPointType point, bool valid) : queue_(queue), point_(point), valid_(valid) {}
public:
    FORCEINLINE explicit operator bool () const & { return valid_; }
    // Make sure that if(RDGCommandHelper::BindXXXShader(...)) does not compile.
    explicit operator bool () const && = delete;

    // Remove copy constructor and copy assignment
    RDGShaderContext (const RDGShaderContext &) = delete;
    RDGShaderContext & operator= (const RDGShaderContext &) = delete;
    // Allow move constructor
    FORCEINLINE RDGShaderContext (RDGShaderContext && other) noexcept : queue_(other.queue_), valid_(other.valid_) {
        other.valid_ = false;
    }
    // Remove move assignment
    RDGShaderContext & operator= (RDGShaderContext && other) = delete;

    ~RDGShaderContext() ;

private:
    bool valid_ = false;
    RHIBindPointType point_;
    RHICommandQueueGraphics & queue_;
};

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
    static RDGShaderContext BindGraphicsShader (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params, bool manual_vbuffer = false) ;

    // Bind a compute shader, setting up required shader parameters and bindings for dispatch commands.
    static RDGShaderContext BindComputeShader (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * compute_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params) ;

    // Bind a raytracing shader, setting up required shader parameters and bindings for dispatch commands.
    static RDGShaderContext BindRayTracingShader (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * ray_tracing_shader,
    const RDGShaderParamStructAndSizeInfo * info, const void * params) ;

    template<CShaderType T>
    FORCEINLINE static RDGShaderContext BindGraphicsShader (
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

    // static void DrawIndexed (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, int index_count, int instance_count = 1, int first_index = 0, int vertex_offset = 0, int first_instance = 0) ;
    // static void DrawIndirect (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, RDGBuffer * indirect_buffer) ;
    // static void DrawIndexedIndirect (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * graphics_shader, RDGBuffer * indirect_buffer) ;

    static void DispatchRays (RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * ray_tracing_shader,
        const RDGShaderParamStructAndSizeInfo * info, const void * params,
        uint32_t width, uint32_t height, uint32_t depth = 1
    ) ;
    template<CShaderType T>
    FORCEINLINE static void DispatchRays (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * ray_tracing_shader, const typename T::ShaderParameters * params,
        uint32_t width, uint32_t height, uint32_t depth = 1
    ) {
        DispatchRays(queue, pass, ray_tracing_shader, T::GetShaderParamStructInfo(), params, width, height, depth);
    }

    static void DispatchRaysIndirect (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * ray_tracing_shader,
        const RDGShaderParamStructAndSizeInfo * info, const void * params, RDGBuffer * indirect_buffer
    ) ;
    template<CShaderType T>
    FORCEINLINE static void DispatchRaysIndirect (
        RHICommandQueueGraphics & queue, RDGPass * pass, RDGShader * ray_tracing_shader, const typename T::ShaderParameters * params,
        RDGBuffer * indirect_buffer
    ) {
        DispatchRaysIndirect(queue, pass, ray_tracing_shader, T::GetShaderParamStructInfo(), params, indirect_buffer);
    }
};

MI_NAMESPACE_END
#endif //RDG_CMD_H
