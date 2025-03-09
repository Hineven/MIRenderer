/*
 * Created: 2025/3/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_SHADER_H
#define RDG_SHADER_H

#include <string>
#include <functional>
#include <rhi/rhi_shader.h>

#include "rdg/rdg_base.h"
#include "rdg/rdg_param.h"
MI_NAMESPACE_BEGIN

class RHIGraphicsPipeline;
class RHIComputePipeline;

struct RDGShaderInitializationInfo {
    RHIPipelineType type;
    std::string source_location;
    std::string compute_entry_;
    std::string vertex_entry_;
    std::string fragment_entry_;
    std::function<RDGShaderParamStructInfo*()> GetShaderParamInfo;
    std::vector<std::string> default_macros;
};

class RDGShaderRegistrator {
public:
    RDGShaderRegistrator (
        size_t type_hash,
        std::function<RDGShaderInitializationInfo()> get_init_info
    ) ;
};

class RDGShader : public RefCounted<true> {
public:
    RDGShader (RDGShaderInitializationInfo ini) ;
    bool Recompile () ;
//    The following functions should be implemented by sub-classes
//  staitc std::vector<std::string> GetDefaultMacros () ;
    FORCEINLINE bool IsValid () const {return is_valid_;}
    FORCEINLINE RHIPipelineType GetType () const {return type_;}
    FORCEINLINE const std::string & GetSourceLocation () const {return source_location_;}
    FORCEINLINE std::string_view GetComputeEntryPoint () const {return shaders_.compute_entry;}
    FORCEINLINE std::string_view GetVertexEntryPoint () const {return shaders_.vertex_entry;}
    FORCEINLINE std::string_view GetFragmentEntryPoint () const {return shaders_.fragment_entry;}
protected:

    // Check if all parameters declared & used in the shader are defined in the shader parameter struct
    void CheckShaderReflection (TRef<RHIShader> shader, const RDGShaderParamStructInfo & info) ;

    // Resource path (infra)
    std::string source_location_ {};
    bool is_valid_ {false};
    RHIPipelineType type_ {};
    TRef<RHIComputePipeline> compute_pipeline_ {nullptr};
    TRef<RHIGraphicsPipeline> graphics_pipeline_ {nullptr};
    struct {
        TRef<RHIShader> compute {};
        std::string compute_entry {};
        TRef<RHIShader> vertex {};
        std::string vertex_entry {};
        TRef<RHIShader> fragment {};
        std::string fragment_entry {};
    } shaders_;

    struct {
        std::function<RDGShaderParamStructInfo*()> GetShaderParamInfo {};
    } child_methods_;
};

// Compute
#define IMPLEMENT_RDG_SHADER(ClassName, SourcePath, Type, EntryPoint) \
    static RDGShaderRegistrator ClassName##Registrator( \
        typeid(ClassName).hash_code(), \
        []() -> RDGShaderInitializationInfo { \
            static_assert(Type == RHIPipelineType::kCompute); \
            return {Type, SourcePath, EntryPoint, "", "" \
            ClassName::GetParamsMetaData}; \
        } \
    );

// Graphics
#define IMPLEMENT_RDG_SHADER(ClassName, SourcePath, Type, EntryPoint_VS, EntryPoint_PS) \
    static RDGShaderRegistrator ClassName##Registrator( \
        typeid(ClassName).hash_code(), \
        []() -> RDGShaderInitializationInfo { \
            static_assert(Type == RHIPipelineType::kGraphics); \
            return {Type, SourcePath, "", EntryPoint_VS, EntryPoint_PS, \
            ClassName::GetParamsMetaData}; \
        } \
    );

#define RDG_SHADER_USE_PARAMETERS(Name) \
    using ShaderParameters = Name; \
    static RDGShaderParamStructInfo GetParamsMetaData() { \
        return ShaderParameters::GetParamsMetaData(); \
    }

class RDGShaderLibrary : public NonMovable, public NonCopyable {
protected:
    RDGShaderLibrary() = default;
public:
    static RDGShaderLibrary & GetInstance() ;
    template<typename T> RDGShader GetShader (std::vector<std::string> macros = {}) ;
    template<typename T> RDGShader Recompile (std::vector<std::string> macros = {}) ;
    void RegisterShader (size_t type_hash, RDGShader * shader) ;
};

MI_NAMESPACE_END
#endif //RDG_SHADER_H
