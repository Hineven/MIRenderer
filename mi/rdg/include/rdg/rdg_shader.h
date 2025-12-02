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
#include "rhi/rhi_pipeline.h"
MI_NAMESPACE_BEGIN
    class RHIGraphicsPipeline;
class RHIComputePipeline;

// Some configuration that can be used to configure the shader pipeline
struct RDGShaderPipelineConfig {
    RHIPrimitiveTopologyType topology {RHIPrimitiveTopologyType::kTriangleList};
    RHIDepthCompareOpType depth_compare_op {RHIDepthCompareOpType::kLess}; // Depth compare operation
    bool depth_write_enabled {true}; // Whether depth write is enabled
    bool depth_test_enabled {true}; // Whether depth test is enabled
    bool rasterization_discard {false}; // Whether to discard all rasterization. Running vertex processing only.
    // std::vector<RHIColorAttachmentDesc> color_attachments;
    struct {
        uint32_t max_recursion_depth {1}; // Maximum ray recursion depth
    } ray_tracing;
};

class RDGShader;

// Struct used to instantiate a shader of a certain class
struct RDGShaderInitializationInfo {
    // Optional macros, the default ones are not included.
    std::vector<std::string> optional_macros;
    size_t GetHash () const ;
};

// Struct used to describe a class of shaders
struct RDGShaderClassRegistry {
    std::string name;
    RHIPipelineType type;
    uint64_t type_hash;
    // Resource path for the source location
    std::string source_location;
    std::string compute_entry_;
    std::string vertex_entry_;
    std::string geometry_entry_;
    std::string fragment_entry_;
    std::string raygen_entry_;
    std::string closest_hit_entry_;
    std::string any_hit_entry_;
    std::string miss_entry_;
    RDGShader * (*Creator) (RDGShaderClassRegistry *);
    // Macros always present when compiling the shader
    std::vector<std::string> (*GetShaderDefaultMacros)();
    // Macros that are optionally present, all possibilities are enumerated when building the shader cache in
    // the shader library.
    // Note: for macros with a value (e.g. "PASS_NUMBER=1"), all possible values of the macro will be automatically
    // collected and enumerated.
    std::vector<std::string> (*GetShaderOptionalMacros)();
    void (*InitShaderParamStructInfo)();
    const RDGShaderParamStructAndSizeInfo * (*GetShaderParamStructInfo)();
    RDGShaderPipelineConfig (*GetShaderPipelineConfig)();
};

struct RDGShaderHash {
    uint64_t value {};
    FORCEINLINE RDGShaderHash () : value(0) {}
    FORCEINLINE explicit RDGShaderHash (uint64_t v) : value(v) {}
    FORCEINLINE bool operator== (const RDGShaderHash & other) const {
        return value == other.value;
    }
    FORCEINLINE bool operator!= (const RDGShaderHash & other) const {
        return value != other.value;
    }
    FORCEINLINE void Reset () {value = 0;}
    RDGShaderHash & AddUnordered (const char * marker, uint64_t v);
};

struct RDGShaderResourceAccess {
    // kNone means that the resource is not used in the shader.
    RHIGPUAccessFlags access {};
    RHIPipelineStageFlags stages {};
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

    // Query the access of a shader resource by its name's crc32 hash.
    // Note: Shader resources include textures, buffers, uniform buffers, samplers.
    // vertex buffers, index buffers, render targets, ... are not shader resources.
    RDGShaderResourceAccess QueryShaderAccess (uint32_t crc32) const ;
    FORCEINLINE RDGShaderResourceAccess QueryShaderAccess (const std::string & resource_name) const {
        return QueryShaderAccess(CRC32(resource_name.c_str(), resource_name.size()));
    }

    bool HasResourceSlot (std::string_view name) const;
    bool HasResourceSlot (uint32_t name_crc) const;

    bool Recompile (RDGShaderInitializationInfo ini) ;

    FORCEINLINE bool IsValid () const {return is_valid_;}

    FORCEINLINE RHIPipelineType GetPipelineType () const {
        return class_registry_->type;
    }

    // Convert the parameter resource index (within its kind) to pipeline slot used for RHI resource binding
    // Returns UINT32_MAX if the index is invalid (e.g. not used in the shader).
    template<RHIParamType type>
    FORCEINLINE uint32_t ConvertParamResourceIndexToResourceSlot (int index) {
        if (index >= (int)cpp_resource_index_to_slot_[(uint32_t)type].size()) return UINT32_MAX;
        return cpp_resource_index_to_slot_[(uint32_t)type][index];
    }


    // The following functions CAN be implemented by sub-classes to specify special shader attributes
    // Default macros that are always present when compiling the shader.
    FORCEINLINE static std::vector<std::string> GetShaderDefaultMacros () {
        return {};
    }
    // Macros that are optionally present, all possibilities are enumerated when building the shader cache in
    // the shader library.
    FORCEINLINE static std::vector<std::string> GetShaderOptionalMacros () {
        return {};
    }
    // Modify the shader pipeline configuration, e.g. topology type.
    FORCEINLINE static RDGShaderPipelineConfig  GetShaderPipelineConfig () {
        return {};
    }

    FORCEINLINE const RDGShaderHash & GetShaderHash () const {
        return shader_hash_;
    }

    // Compute shader hash from infra resources. The hash value will be different if the shader source code
    // or compile options are changed.
    RDGShaderHash ComputeShaderHash () const ;

    const RDGShaderClassRegistry * GetShaderClassRegistry () const {
        return class_registry_;
    }

    // Transfer the thread owner for underlying RHI resources.
    void UpdateOwnerForRHIResources ();

    struct SBTBuffers {
        RHIBufferSpan raygen;
        RHIBufferSpan miss;
        RHIBufferSpan hit;
        uint64_t miss_stride;
        uint64_t hit_stride;
    };

    SBTBuffers GetSBTBuffers (RHICommandQueueGraphics & queue) ;

    FORCEINLINE const std::string & GetName () const {
        return class_registry_->name;
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

    // Shortcut
    // Get the resource slot by the name of the resource reflected from shaders
    RHIPipelineResourceSlot ReflectResourceSlot (std::string_view name) const;
    RHIPipelineResourceSlot ReflectResourceSlot (uint32_t name_crc) const;

    // Get the extra compiler options for the shader with the given initialization info
    std::vector<std::string> GetExtraCompilerOptions (const RDGShaderInitializationInfo & ini) const;

    // Get the extra defines for the shader with the given initialization info
    std::vector<std::string> GetExtraDefines (const RDGShaderInitializationInfo & ini) const;

    // Clear and rebuild the mapping between cpp resource indices and pipeline slots
    void RemapResourceIndexToRHIResourceSlots ();

    std::string LoadSource () const ;
    // Helper function, re-compile shaders only.
    bool RecompileShaders (const std::string & source_code, const RDGShaderInitializationInfo & ini) ;

    // Check if all parameters declared & used in the shader are defined in the shader parameter struct
    bool CheckShaderReflection (RHIShader * shader, const RDGShaderParamStructAndSizeInfo & info) ;

    // Shader (compile options + source with expanded includes) hash value (xxhash64), used for checking if the shader source has changed
    RDGShaderHash shader_hash_;

    // Resource path (infra)
    bool is_valid_ {false};
    TRef<RHIComputePipeline> compute_pipeline_;
    TRef<RHIGraphicsPipeline> graphics_pipeline_;
    TRef<RHIRayTracingPipeline> ray_tracing_pipeline_;
    struct {
        TRef<RHIShader> compute {};
        TRef<RHIShader> vertex {};
        TRef<RHIShader> geometry {};
        TRef<RHIShader> fragment {};
        TRef<RHIShader> raygen {};
        TRef<RHIShader> miss {};
        TRef<RHIShader> closest_hit {};
        TRef<RHIShader> any_hit {};
        TRef<RHIShader> callable {};
    } shaders_;
    // SBT buffer only available for ray tracing shaders.
    // They are generated on-the fly when requested for the first time.
    TRef<RHIBuffer> sbt_buffer_ {};
    std::vector<std::byte> sbt_;
    struct {
        struct Section {
            size_t offset {};
            size_t size {};
        };
        Section raygen, hit, miss;
    } sbt_sections_;

    RDGShaderPipelineConfig pipeline_config_ {};
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

template<typename T, typename = void>
struct TGetShaderOptionalMacros {
    constexpr static auto value = RDGShader::GetShaderOptionalMacros;
};
template<typename T>
struct TGetShaderOptionalMacros<T, std::void_t<decltype(T::GetShaderOptionalMacros)>> {
    constexpr static auto value = T::GetShaderOptionalMacros;
};

// Helpers
#define MI_PP_GET_FIRST(first, ...) first
#define MI_PP_HAS_ARGS_IMPL(...) MI_PP_GET_FIRST(__VA_ARGS__ 0)
#define MI_PP_HAS_ARGS(...) MI_PP_HAS_ARGS_IMPL(__VA_OPT__(1,))

#define MI_PP_CAT(a, b) MI_PP_CAT_I(a, b)
#define MI_PP_CAT_I(a, b) a##b


#define DECLARE_SHADER_0() \
protected: \
using RDGShader::RDGShader; \
public: \
template<typename T> friend class ::MI_NAMESPACE::RDGShaderClassRegistrator; \
friend class ::MI_NAMESPACE::RDGShaderLibrary; \
static RDGPassType GetRDGPassType (); \
static const char * GetShaderTypeName ();

#define DECLARE_SHADER_1(Super) \
protected: \
using Super::Super; \
public: \
template<typename T> friend class ::MI_NAMESPACE::RDGShaderClassRegistrator; \
friend class ::MI_NAMESPACE::RDGShaderLibrary; \
static RDGPassType GetRDGPassType (); \
static const char * GetShaderTypeName ();

#define DECLARE_SHADER_SELECT(n) MI_PP_CAT(DECLARE_SHADER_, n)
#define DECLARE_SHADER(...) DECLARE_SHADER_SELECT(MI_PP_HAS_ARGS(__VA_ARGS__))(__VA_ARGS__)

// Generic
#define INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, Type, EntryPoint_CS, EntryPoint_VS, EntryPoint_GS, EntryPoint_PS, EntryPoint_Raygen, EntryPoint_ClosestHit, EntryPoint_AnyHit, EntryPoint_Miss) \
    static RDGShaderClassRegistrator<ClassName> ClassName##Registrator( \
        #ClassName, \
        Type,\
        typeid(ClassName).hash_code(), \
        SourcePath, \
        EntryPoint_CS, \
        EntryPoint_VS, \
        EntryPoint_GS, \
        EntryPoint_PS, \
        EntryPoint_Raygen, \
        EntryPoint_ClosestHit, \
        EntryPoint_AnyHit, \
        EntryPoint_Miss \
    ); \
    RDGPassType ClassName::GetRDGPassType () {return ::MI_NAMESPACE::GetRDGPassType(Type);} \
    const char * ClassName::GetShaderTypeName () {return #ClassName;} \

// Compute
#define IMPLEMENT_RDG_COMPUTE_SHADER(ClassName, SourcePath, EntryPoint_CS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kCompute, EntryPoint_CS, "", "", "", "", "", "", "") \
    IMPLEMENT_SHADER_PARAMETERS(ClassName::ShaderParameters)

// Graphics
#define IMPLEMENT_RDG_GRAPHICS_SHADER(ClassName, SourcePath, EntryPoint_VS, EntryPoint_PS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kGraphics, "", EntryPoint_VS, "", EntryPoint_PS, "", "", "", "") \
    IMPLEMENT_SHADER_PARAMETERS(ClassName::ShaderParameters)

// Added: Graphics shader variant with geometry stage (non-shared parameter struct)
#define IMPLEMENT_RDG_GRAPHICS_SHADER_GS(ClassName, SourcePath, EntryPoint_VS, EntryPoint_GS, EntryPoint_PS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kGraphics, "", EntryPoint_VS, EntryPoint_GS, EntryPoint_PS, "", "", "", "") \
    IMPLEMENT_SHADER_PARAMETERS(ClassName::ShaderParameters)

// Ray tracing
#define IMPLEMENT_RDG_RAY_TRACING_SHADER(ClassName, SourcePath, EntryPoint_Raygen, EntryPoint_ClosestHit, EntryPoint_AnyHit, EntryPoint_Miss) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kRayTracing, "", "", "", "", EntryPoint_Raygen, EntryPoint_ClosestHit, EntryPoint_AnyHit, EntryPoint_Miss) \
    IMPLEMENT_SHADER_PARAMETERS(ClassName::ShaderParameters)

// For shaders using shared parameter structs among multiple shaders, use this macro along with IMPLEMENT_SHADER_PARAMETERS(ParamStructName)
#define IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClassName, SourcePath, EntryPoint_CS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kCompute, EntryPoint_CS, "", "", "", "", "", "", "") \

// For shaders using shared parameter structs among multiple shaders, use this macro along with IMPLEMENT_SHADER_PARAMETERS(ParamStructName)
#define IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER(ClassName, SourcePath, EntryPoint_VS, EntryPoint_PS) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kGraphics, "", EntryPoint_VS, "", EntryPoint_PS, "", "", "", "") \

#define IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER_GS(ClassName, SourcePath, EntryPoint_VS, EntryPoint_GS, EntryPoint_PS) \
INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kGraphics, "", EntryPoint_VS, EntryPoint_GS, EntryPoint_PS, "", "", "", "") \

// For shaders using shared parameter structs among multiple shaders, use this macro along with IMPLEMENT_SHADER_PARAMETERS(ParamStructName)
#define IMPLEMENT_RDG_RAY_TRACING_SHADER_SHADER_SHARED_PARAMETER(ClassName, SourcePath, EntryPoint_Raygen, EntryPoint_ClosestHit, EntryPoint_AnyHit, EntryPoint_Miss) \
    INTERNAL_IMPLEMENT_RDG_SHADER(ClassName, SourcePath, RHIPipelineType::kRayTracing, "", "", "", "", EntryPoint_Raygen, EntryPoint_ClosestHit, EntryPoint_AnyHit, EntryPoint_Miss) \


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
    void Deinit ();

    // Release all compiled shaders of all shader classes.
    // Further requests of any shader will invoke a re-compile.
    void ReleaseCompiledShaders();

    template<typename T>
    friend class RDGShaderClassRegistrator;
    static RDGShaderLibrary & Get() ;

    // Check all shaders and recompile the modified ones.
    // Should only be performed when RHI is idle.
    void RecompileUpdatedCachedShaders () ;

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
    std::map<size_t, std::unique_ptr<RDGShaderClassRegistry>> registered_shader_classes_ {};
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
        uint64_t type_hash,
        const std::string & source_location,
        const std::string & compute_entry,
        const std::string & vertex_entry,
        const std::string & geometry_entry,
        const std::string & fragment_entry,
        const std::string & raygen_entry,
        const std::string & closest_hit_entry,
        const std::string & any_hit_entry,
        const std::string & miss_entry
    ) {
        auto & lib = RDGShaderLibrary::Get();
        auto registry = RDGShaderClassRegistry {
            name,
            type,
            type_hash,
            source_location,
            compute_entry,
            vertex_entry,
            geometry_entry,
            fragment_entry,
            raygen_entry,
            closest_hit_entry,
            any_hit_entry,
            miss_entry,
            RDGShaderClassRegistrator<T>::zzShaderFactoryFunction,
            TGetShaderDefaultMacros<T>::value,
            TGetShaderOptionalMacros<T>::value,
            T::ShaderParameters::InitParamStructInfo,
            T::GetShaderParamStructInfo,
            TGetShaderPipelineConfig<T>::value
        };
        lib.RegisterShaderClass(typeid(T).hash_code(), registry);
    }
protected:
    FORCEINLINE static RDGShader * zzShaderFactoryFunction (RDGShaderClassRegistry * registry) {
        auto shader = (RDGShader*)(new T(registry));
        return shader;
    }
};

MI_NAMESPACE_END
#endif //RDG_SHADER_H
