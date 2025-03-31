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
    std::vector<std::string> (*GetShaderDefaultMacros)();
    const RDGShaderParamStructAndSizeInfo * (*GetShaderParamStructInfo)();
    RDGShaderPipelineConfig (*GetShaderPipelineConfig)();
};

class RDGShader : public RefCounted<true> {
protected:
    RDGShader (const RDGShaderClassRegistry * class_registry) ;
    virtual ~RDGShader() ;
public:

    template<typename T>
    friend class RDGShaderClassRegistrator;
    friend class RDGShaderLibrary;
    friend class RDGCommandHelper;

    bool Recompile (RDGShaderInitializationInfo ini) ;

    FORCEINLINE bool IsValid () const {return is_valid_;}

    // Convert the parameter resource index (within its kind) to pipeline slot used for RHI resource binding
    template<RHIParamType type>
    FORCEINLINE uint32_t ConvertParamResourceIndexToResourceSlot (int index) {
        return cpp_resource_index_to_slot_[(uint32_t)type][index];
    }


    // The following functions CAN be implemented by sub-classes to specify special shader attributes
    FORCEINLINE static std::vector<std::string> GetShaderDefaultMacros () {
        return {};
    }
    FORCEINLINE static RDGShaderPipelineConfig  GetShaderPipelineConfig () {
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

    // Mapping resource indices in cpp declaration (essentially the index of the resource in the top level cpp info struct)
    // to pipeline slot numbers (used for resource binding) reflected via pipeline compilation.
    // Note: special case, global uniform buffer have index ref_uniform_buffers.size() in the kUniformBuffer vector.
    // (the last element in the vector)
    std::vector<uint32_t> cpp_resource_index_to_slot_[(uint32_t)RHIParamType::kMax];
    // Clear and rebuild the mapping between cpp resource indices and pipeline slots
    void RemapResourceIndexToResourceSlots ();



    std::string LoadSource () const ;
    // Helper function, re-compile shaders only.
    bool RecompileShaders (const std::string & source_code) ;

    // Check if all parameters declared & used in the shader are defined in the shader parameter struct
    bool CheckShaderReflection (RHIShader * shader, const RDGShaderParamStructAndSizeInfo & info) ;


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
    constexpr static auto value = RDGShader::GetShaderPipelineConfig;
};
template<typename T>
struct TGetShaderPipelineConfig<T, std::void_t<decltype(T::GetShaderPipelineConfig)>> {
    constexpr static auto value = T::GetShaderPipelineConfig;
};

template<typename T, typename = void>
struct TGetShaderDefaultMacros {
    constexpr static auto value = RDGShader::GetShaderDefaultMacros;
};
template<typename T>
struct TGetShaderDefaultMacros<T, std::void_t<decltype(T::GetShaderDefaultMacros)>> {
    constexpr static auto value = T::GetShaderDefaultMacros;
};

#define DECLARE_SHADER() \
protected: \
    using RDGShader::RDGShader; \
public: \
    template<typename T> friend class RDGShaderClassRegistrator; \
    friend class RDGShaderLibrary; \
    static RDGPassType GetRDGPassType () ; \
    static const char * GetShaderTypeName () ; \

// Generic
#define INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, Type, EntryPoint_CS, EntryPoint_VS, EntryPoint_PS) \
    static RDGShaderClassRegistrator<ClassName> ClassName##Registrator( \
        #ClassName, \
        Type,\
        SourcePath, \
        EntryPoint_CS, \
        EntryPoint_VS, \
        EntryPoint_PS \
    ); \
    RDGPassType ClassName::GetRDGPassType () {return ::MI_NAMESPACE::GetRDGPassType(Type);} \
    const char * ClassName::GetShaderTypeName () {return #ClassName;}

// Compute
#define IMPLEMENT_RDG_COMPUTE_SHADER(ClassName, SourcePath, EntryPoint_CS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kCompute, EntryPoint_CS, "", "")

// Graphics
#define IMPLEMENT_RDG_GRAPHICS_SHADER(ClassName, SourcePath, EntryPoint_VS, EntryPoint_PS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kGraphics, "", EntryPoint_VS, EntryPoint_PS)

#define RDG_SHADER_USE_PARAMETERS(Name) \
public: \
    using ShaderParameters = Name; \
    static const RDGShaderParamStructAndSizeInfo * GetShaderParamStructInfo() { \
        return ShaderParameters::GetParamStructInfo(); \
    }

class RDGShaderLibrary : public NonMovable, public NonCopyable {
protected:
    RDGShaderLibrary() = default;
    ~RDGShaderLibrary() ;
public:
    void Init ();

    // Release all compiled shaders of all shader classes.
    // Further requests of any shader will invoke a re-compile.
    void ReleaseCompiledShaders();

    template<typename T>
    friend class RDGShaderClassRegistrator;
    static RDGShaderLibrary & Get() ;
    static void DestroySingleton () ;

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
        const std::string & fragment_entry
    ) {
        auto & lib = RDGShaderLibrary::Get();
        auto registry = RDGShaderClassRegistry {
            name,
            type,
            source_location,
            compute_entry,
            vertex_entry,
            fragment_entry,
            RDGShaderClassRegistrator<T>::zzShaderFactoryFunction,
            TGetShaderDefaultMacros<T>::value,
            T::GetShaderParamStructInfo,
            TGetShaderPipelineConfig<T>::value
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
