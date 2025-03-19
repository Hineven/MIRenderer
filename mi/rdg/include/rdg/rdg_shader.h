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

class RDGShader;

// Struct used to instantiate a shader of a certain class
struct RDGShaderInitializationInfo {
    std::vector<std::string> macros;
    size_t GetHash () const ;
};

// Struct used to describe a class of shaders
struct RDGShaderClassRegistry {
    std::string name;
    RHIPipelineType type;
    std::string source_location;
    std::string compute_entry_;
    std::string vertex_entry_;
    std::string fragment_entry_;
    RDGShader * (*Creator) (RDGShaderClassRegistry *, RDGShaderInitializationInfo);
    const RDGShaderParamStructAndSizeInfo * (*GetShaderParamStructInfo)();
    RDGShaderPipelineConfig (*GetShaderPipelineConfig)();
};

class RDGShader : public RefCounted<true> {
protected:
    FORCEINLINE RDGShader (const RDGShaderClassRegistry * class_registry) : class_registry_(class_registry) {}
    virtual ~RDGShader() ;
public:
    template<typename T>
    friend class RDGShaderClassRegistrator;
    friend class RDGShaderLibrary;
    bool Recompile (RDGShaderInitializationInfo ini) ;
//    The following functions should be implemented by sub-classes
//  staitc std::vector<std::string> GetDefaultMacros () ;
    FORCEINLINE bool IsValid () const {return is_valid_;}

    FORCEINLINE static RDGShaderPipelineConfig GetDefaultShaderPipelineConfig () {
        return RDGShaderPipelineConfig {
            RHIPrimitiveTopologyType::kTriangleList
        };
    }

protected:

    // Shader initialization info (default, given in constructor)
    RDGShaderInitializationInfo ini_ {
        {}
    };
    // Point to the class registry deriving the shader
    const RDGShaderClassRegistry * class_registry_;

    std::string LoadSource () const ;
    // Helper function, re-compile shaders only.
    bool RecompileShaders (const std::string & source_code) ;

    // Check if all parameters declared & used in the shader are defined in the shader parameter struct
    bool CheckShaderReflection (TRef<RHIShader> shader, const RDGShaderParamStructInfo & info) const ;

    // Resource path (infra)
    bool is_valid_ {false};
    TRef<RHIComputePipeline> compute_pipeline_;
    TRef<RHIGraphicsPipeline> graphics_pipeline_;
    struct {
        TRef<RHIShader> compute {};
        TRef<RHIShader> vertex {};
        TRef<RHIShader> fragment {};
    } shaders_;
};

template<typename T, typename = void>
struct TGetShaderPipelineConfig {
    constexpr static auto value = RDGShader::GetDefaultShaderPipelineConfig;
};

template<typename T>
struct TGetShaderPipelineConfig<T, std::void_t<decltype(T::GetShaderPipelineConfig)>> {
    constexpr static auto value = T::GetShaderPipelineConfig;
};

#define DECLARE_SHADER() \
    template<typename T> friend class RDGShaderClassRegistrator; \
    friend class RDGShaderLibrary; \
    using RDGShader::RDGShader;

// Generic
#define IMPLEMENT_RDG_GENERIC_SHADER(ClassName, SourcePath, Type, EntryPoint_CS, EntryPoint_VS, EntryPoint_PS) \
    static RDGShaderClassRegistrator<ClassName> ClassName##Registrator( \
        #ClassName, \
        Type,\
        SourcePath, \
        EntryPoint_CS, \
        EntryPoint_VS, \
        EntryPoint_PS, \
        ClassName::GetParamStructInfo, \
        TGetShaderPipelineConfig<ClassName>::value \
    );

// Compute
#define IMPLEMENT_RDG_COMPUTE_SHADER(ClassName, SourcePath, EntryPoint_CS) \
    IMPLEMENT_RDG_GENERIC_SHADER(ClassName, SourcePath, RHIPipelineType::kCompute, EntryPoint_CS, "", "")

// Graphics
#define IMPLEMENT_RDG_GRAPHICS_SHADER(ClassName, SourcePath, EntryPoint_VS, EntryPoint_PS) \
    IMPLEMENT_RDG_GENERIC_SHADER(ClassName, SourcePath, RHIPipelineType::kGraphics, "", EntryPoint_VS, EntryPoint_PS)

#define RDG_SHADER_USE_PARAMETERS(Name) \
    using ShaderParameters = Name; \
    static const RDGShaderParamStructAndSizeInfo * GetParamStructInfo() { \
        return ShaderParameters::GetParamStructInfo(); \
    }

class RDGShaderLibrary : public NonMovable, public NonCopyable {
protected:
    RDGShaderLibrary() = default;
    ~RDGShaderLibrary() ;
public:
    void Init ();

    template<typename T>
    friend class RDGShaderClassRegistrator;
    static RDGShaderLibrary & GetInstance() ;
    template<typename T>
    FORCEINLINE T * GetShader (RDGShaderInitializationInfo ini = {}) {
        return (T*)GetShader(typeid(T).hash_code(), ini);
    }
    RDGShader * GetShader (size_t type_hash, RDGShaderInitializationInfo ini = {}) ;
protected:
    void RegisterShaderClass (size_t type_hash, RDGShaderClassRegistry registry) ;

    FORCEINLINE void DeleteShader (RDGShader * shader) {
        delete shader;
    }
    struct DeleteShaderType {
        void operator() (RDGShader * shader) const {
            delete shader;
        }
    };

    // Shader creators
    std::map<size_t, std::unique_ptr<RDGShaderClassRegistry>> registered_shaders_ {};
    // Compiled shaders
    // REMEMBER to delete shaders when removing them.
    std::map<size_t, std::unique_ptr<RDGShader, DeleteShaderType>> cached_shaders_ {};
};

// Registrator for a class of shaders
template<typename T>
class RDGShaderClassRegistrator {
public:
    FORCEINLINE RDGShaderClassRegistrator (
        std::string name,
        RHIPipelineType type,
        const std::string & source_location,
        const std::string & compute_entry,
        const std::string & vertex_entry,
        const std::string & fragment_entry,
        const RDGShaderParamStructAndSizeInfo * (*GetShaderParamStructInfo)(),
        RDGShaderPipelineConfig (*GetShaderPipelineConfig)()
    ) {
        auto & lib = RDGShaderLibrary::GetInstance();
        auto registry = RDGShaderClassRegistry {
            name,
            type,
            source_location,
            compute_entry,
            vertex_entry,
            fragment_entry,
            RDGShaderClassRegistrator<T>::zzShaderFactoryFunction,
            GetShaderParamStructInfo,
            GetShaderPipelineConfig,
        };
        lib.RegisterShaderClass(typeid(T).hash_code(), registry);
    }
protected:
    FORCEINLINE static RDGShader * zzShaderFactoryFunction (RDGShaderClassRegistry * registry, RDGShaderInitializationInfo info) {
        auto shader = (RDGShader*)(new T(registry));
        shader->Recompile(info);
        return shader;
    }
};

MI_NAMESPACE_END
#endif //RDG_SHADER_H
