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

struct RDGShaderPipelineConfig {
    RHIPrimitiveTopologyType topology {};
    // std::vector<RHIColorAttachmentDesc> color_attachments;
};

struct RDGShaderInitializationInfo {
    std::string name;
    RHIPipelineType type;
    std::string source_location;
    std::string compute_entry_;
    std::string vertex_entry_;
    std::string fragment_entry_;
    std::function<RDGShaderParamStructAndSizeInfo*()> GetParamStructInfo;
    std::vector<std::string> default_macros;
    std::function<RDGShaderPipelineConfig()> GetShaderPipelineConfig;
};

class RDGShaderRegistrator {
public:
    RDGShaderRegistrator (
        size_t type_hash,
        std::function<RDGShaderInitializationInfo()> get_init_info
    ) ;
};

class RDGShader : public RefCounted<true> {
protected:
    struct ShaderEntries {
        std::string compute {};
        std::string vertex {};
        std::string fragment {};
    } shader_entries_ ;
public:
    RDGShader (RDGShaderInitializationInfo ini) ;
    bool Recompile (RDGShaderParamStructAndSizeInfo * info) ;
//    The following functions should be implemented by sub-classes
//  staitc std::vector<std::string> GetDefaultMacros () ;
    FORCEINLINE bool IsValid () const {return is_valid_;}
    FORCEINLINE RHIPipelineType GetType () const {return type_;}
    FORCEINLINE const std::string & GetSourceLocation () const {return source_location_;}
    FORCEINLINE const ShaderEntries & GetShaderEntries () const {return shader_entries_;}
    FORCEINLINE const std::string & GetName () const {return name_;}

    FORCEINLINE static RDGShaderPipelineConfig GetDefaultShaderPipelineConfig () {
        return RDGShaderPipelineConfig {
            RHIPrimitiveTopologyType::kTriangleList
        };
    }

protected:

    std::string LoadSource () const ;

    // Helper function, re-compile shaders only.
    bool RecompileShaders (const std::string & source_code) ;

    std::string name_ {"<unknown>"};
    // Check if all parameters declared & used in the shader are defined in the shader parameter struct
    bool CheckShaderReflection (TRef<RHIShader> shader, const RDGShaderParamStructInfo & info) const ;

    // Resource path (infra)
    std::string source_location_ {};
    bool is_valid_ {false};
    RHIPipelineType type_ {};
    TRef<RHIComputePipeline> compute_pipeline_ {nullptr};
    TRef<RHIGraphicsPipeline> graphics_pipeline_ {nullptr};
    struct {
        TRef<RHIShader> compute {};
        TRef<RHIShader> vertex {};
        TRef<RHIShader> fragment {};
    } shaders_;

    struct {
        std::function<RDGShaderParamStructAndSizeInfo*()> GetParamStructInfo {};
        std::function<RDGShaderPipelineConfig()> GetShaderPipelineConfig {};
    } child_methods_;
};

template<typename T, typename = void>
struct TGetShaderPipelineConfig {
    constexpr auto value = RDGShader::GetDefaultShaderPipelineConfig;
};

template<typename T>
struct TGetShaderPipelineConfig<T, std::void_t<decltype(T::GetShaderPipelineConfig)>> {
    constexpr auto value = T::GetShaderPipelineConfig;
};

// Compute
#define IMPLEMENT_RDG_COMPUTE_SHADER(ClassName, SourcePath, Type, EntryPoint) \
    static RDGShaderRegistrator ClassName##Registrator( \
        typeid(ClassName).hash_code(), \
        []() -> RDGShaderInitializationInfo { \
            static_assert(Type == RHIPipelineType::kCompute); \
            return {#ClassName, \
            Type, SourcePath, \
            EntryPoint, "", "", \
            ClassName::GetParamStructInfo, \
            TGetShaderPipelineConfig<ClassName>::value}; \
        } \
    );

// Graphics
#define IMPLEMENT_RDG_GRAPHICS_SHADER(ClassName, SourcePath, Type, EntryPoint_VS, EntryPoint_PS) \
    static RDGShaderRegistrator ClassName##Registrator( \
        typeid(ClassName).hash_code(), \
        []() -> RDGShaderInitializationInfo { \
            static_assert(Type == RHIPipelineType::kGraphics); \
            return { \
                #ClassName, \
                Type, SourcePath, \
                "", EntryPoint_VS, EntryPoint_PS, \
                ClassName::GetParamStructInfo, \
                TGetShaderPipelineConfig<ClassName>::value}; \
        } \
    );

#define RDG_SHADER_USE_PARAMETERS(Name) \
    using ShaderParameters = Name; \
    static const RDGShaderParamStructAndSizeInfo * GetParamStructInfo() { \
        return ShaderParameters::GetParamStructInfo(); \
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
