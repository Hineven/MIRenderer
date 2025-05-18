/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PARAM_H
#define RDG_PARAM_H

#include <array>
#include <map>
#include <regex>
#include <set>
#include <utility>
#include <core/constants.h>

#include "core/crc.h"
#include "core/infra.h"
#include "core/pixel_format.h"
#include "rhi/rhi_param.h"
#include "rhi/rhi_fwd.h"
#include "rdg/rdg_base.h"

MI_NAMESPACE_BEGIN

struct RDGShaderParamStructAndSizeInfo;

struct RDGImportedShaderParamStructInfo {
    const RDGShaderParamStructAndSizeInfo * cpp_struct_info {nullptr};
};

struct RDGShaderRenderTargetInfo {
    uint32_t target_index; // UINT32_MAX for depth stencil
    PixelFormatType format;
};

struct RDGShaderVertexBufferInfo {
    uint32_t index;
    uint32_t stride;
};

struct RDGShaderVertexAttributeInfo {
    // Reference to the index of a vertex buffer
    uint32_t buffer_index;
    // offset of the attribute
    uint32_t offset;
    // Attribute index / location
    uint32_t attribute_index;
    // Attribute input format
    RHIVertexAttributeFormatType format;
};

struct RDGShaderIndexBufferInfo {
    // Index buffer format
    RHIIndexType format;
};

// C++ side shader parameter + shader side shader parameter pair
struct RDGShaderParamInfo : public RHIParamInfo {
    // (cpp side) Specify how the parameter lives in the shader parameter struct.
    uint32_t cpp_offset {UINT32_MAX};
    // (cpp side) The struct info of the parameter if it is a struct.
    RDGImportedShaderParamStructInfo cpp_imported_struct_info {};
    // Extra info about render target / vertex buffer / vertex attribute / index buffer
    union {
        RDGShaderRenderTargetInfo * render_targets_info;
        RDGShaderVertexBufferInfo * vertex_buffer_info;
        RDGShaderVertexAttributeInfo * vertex_attribute_info;
        // dispatch command does not have extra info.
    } cpp_extra {};
};

// C++ side shader parameter struct + shader side shader parameter struct pair
struct RDGShaderParamStructInfo : public RHIParamStructInfo {
    std::span<RDGShaderParamInfo> cpp_members {};
    // Whether this struct can be imported to another RDGShaderParamStructInfo and wrapped by a RDGImportedShaderParamStructInfo
    // If this struct does not contain any references and shader resources, it can be imported.
    bool CanBeImported () const ;
    // Whether this struct describes a renderpass. A renderpass should only be described via a series of render targets.
    bool CanBeRenderpass () const;
    std::map<uint32_t /*CRC*/, int> cpp_member_index_map;
    // Get the index of the member within all members (cpp_members). (Including pseudo-members such as vertex attributes)
    FORCEINLINE int GetCppMemberIndex(uint32_t crc) const {
        auto it = cpp_member_index_map.find(crc);
        if(it == cpp_member_index_map.end()) return -1;
        return it->second;
    }
    // Get the index of the member within all members (cpp_members). (Including pseudo-members such as vertex attributes)
    FORCEINLINE int GetCppMemberIndex (const char * name) const {
        return GetCppMemberIndex(CRC32String(name));
    }
    // Get the index of the member within all members (cpp_members). (Including pseudo-members such as vertex attributes)
    FORCEINLINE int GetCppMemberIndex (std::string_view name) const {
        return GetCppMemberIndex(CRC32String(name));
    }
};

struct RDGShaderParameterLocation {
    const RDGShaderParamInfo * info;
    uint32_t cpp_offset;
    uint32_t shader_offset;
    uint32_t size;
};

// TODO rename this
struct RDGShaderParamStructAndSizeInfo: public RDGShaderParamStructInfo {
    // Cache the device size of the parameter struct at the outer most level
    uint32_t size;
    // Some other data to support fast reflection checking and parameter filling
    std::span<RDGShaderParameterLocation> global_uniforms_;
    std::span<RDGShaderParameterLocation> storage_buffers_;
    std::span<RDGShaderParameterLocation> uniform_buffers_;
    std::span<RDGShaderParameterLocation> uavs_;
    std::span<RDGShaderParameterLocation> srvs_;
    std::span<RDGShaderParameterLocation> samplers_;
    std::span<RDGShaderParameterLocation> acceleration_structures_;
    // Fast reflection for graphics pipelines
    std::span<RDGShaderParameterLocation> vertex_buffers_;
    std::span<RDGShaderParameterLocation> vertex_attributes_;
    RDGShaderParameterLocation index_buffer_;
    std::span<RDGShaderParameterLocation> render_targets_;
    RDGShaderParameterLocation dispatch_command_;
    RDGShaderParameterLocation renderpass_;
};

// Map hlsl type strings to C++ metadata and types
template<uint32_t CRC> struct TRDGShaderParamPlaceHolderType;
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int")> {
    typedef int value;
    FORCEINLINE static int default_value() {return 0;}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int2")> {
    typedef glm::ivec2 value;
    FORCEINLINE static glm::ivec2 default_value() {return {0, 0};}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int3")> {
    typedef glm::ivec3 value;
    FORCEINLINE static glm::ivec3 default_value() {return {0, 0, 0};}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int4")> {
    typedef glm::ivec4 value;
    FORCEINLINE static glm::ivec4 default_value() {return {0, 0, 0, 0};}
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint")> {
    typedef uint32_t value;
    FORCEINLINE static uint32_t default_value() {return 0;}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint2")> {
    typedef glm::uvec2 value;
    FORCEINLINE static glm::uvec2 default_value() {return {0, 0};}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint3")> {
    typedef glm::uvec3 value;
    FORCEINLINE static glm::uvec3 default_value() {return {0, 0, 0};}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint4")> {
    typedef glm::uvec4 value;
    FORCEINLINE static glm::uvec4 default_value() {return {0, 0, 0, 0};}
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float")> {
    typedef float value;
    FORCEINLINE static float default_value() {return 0.0f;}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float2")> {
    typedef glm::vec2 value;
    FORCEINLINE static glm::vec2 default_value() {return {0.0f, 0.0f};}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float3")> {
    typedef glm::vec3 value;
    FORCEINLINE static glm::vec3 default_value() {return {0.0f, 0.0f, 0.0f};}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float4")> {
    typedef glm::vec4 value;
    FORCEINLINE static glm::vec4 default_value() {return {0.0f, 0.0f, 0.0f, 0.0f};}
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float4x4")> {
    typedef glm::mat4 value;
    FORCEINLINE static glm::mat4 default_value() {return {0.0f};}
};

struct RDGShaderTextureParameter {
    // Keep this as the first member because somewhere in my code may use *(RDGTexture**)(ptr+offset)
    // as a way to access the texture pointer (for legacy reasons).
    RDGTexture * texture {};
    // The layer of the texture to bind to. This is used for 3D textures and cube maps.
    // Leave UINT_MAX for whole texture arrays / defaults. And for Texture2DArray and TextureCube, this has to be UINT_MAX.
    uint32_t array_layer {UINT_MAX};
    FORCEINLINE operator RDGTexture * () const { return texture; }
    FORCEINLINE RDGShaderTextureParameter & operator = (RDGTexture * tex) {
        texture = tex;
        return *this;
    }
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("Texture2D")> {
    typedef RDGShaderTextureParameter value;
    FORCEINLINE static RDGTexture * default_value() {return reinterpret_cast<RDGTexture*>(RDGParameter_UnsetPointer);}
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("TextureCube")>
    : public TRDGShaderParamPlaceHolderType<ConstStrHash32("Texture2D")> {};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("Texture2DArray")>
: public TRDGShaderParamPlaceHolderType<ConstStrHash32("Texture2D")> {};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWTexture2D")>
: public TRDGShaderParamPlaceHolderType<ConstStrHash32("Texture2D")> {};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("SamplerState")> {
    typedef RHISampler * value;
    FORCEINLINE static RHISampler * default_value() {return reinterpret_cast<RHISampler*>(RDGParameter_UnsetPointer);}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("Buffer")> {
    typedef RDGBuffer * value;
    FORCEINLINE static RDGBuffer * default_value() {return reinterpret_cast<RDGBuffer*>(RDGParameter_UnsetPointer);}
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("StructuredBuffer")>
: public TRDGShaderParamPlaceHolderType<ConstStrHash32("Buffer")> {};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWBuffer")>
: public TRDGShaderParamPlaceHolderType<ConstStrHash32("Buffer")> {};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWStructuredBuffer")>
: public TRDGShaderParamPlaceHolderType<ConstStrHash32("Buffer")> {};

FORCEINLINE RDGShaderParamInfo RDGMakeShaderParamInfo (
    const std::string& type_name, std::string param_name, uint32_t cpp_offset,
    const RDGShaderParamStructAndSizeInfo * cpp_struct_info = nullptr, bool ub_reference = false, bool renderpass = false) {
    RDGShaderParamInfo info {};
    info.name = std::move(param_name);
    info.type = RHITypeNameStringToParamType(type_name);
    if (cpp_struct_info) {
        // In this case, modify the type to UniformBuffer for uniform buffer reference.
        if (ub_reference) info.type = RHIParamType::kUniformBuffer;
        // This is a render pass parameter
        if (renderpass) info.type = RHIParamType::kRenderPass;
    }
    if (renderpass) info.access_flags = RHIGPUAccessFlagBits::kRW;
    else info.access_flags = TypeNameStringToRHIAccessFlags(type_name);

    if(info.type == RHIParamType::kBasic) {
        info.basic_type = RHITypeNameStringToBasicParamType(type_name);
    }
    if (info.type == RHIParamType::kStruct
    || info.type == RHIParamType::kUniformBuffer
    || info.type == RHIParamType::kRenderPass) {
        info.struct_info = cpp_struct_info;
        info.cpp_imported_struct_info.cpp_struct_info = cpp_struct_info;
    }
    // info.offset = offset; // Offsets will be assigned when finalizing
    info.cpp_offset = cpp_offset;
    if (info.struct_info) {
        info.size = cpp_struct_info->size;
    } else if (info.type == RHIParamType::kBasic) {
        info.size = RHIGetBasicParamSize(info.basic_type);
    }
    return info;
}

namespace details {
    // Compute the device-side uniform buffer size of the parameter struct
    bool zzFinalizeParams(std::vector<RDGShaderParamInfo> & params);
    void zzFinalizeTopLevelParamsStructInfo (RDGShaderParamStructAndSizeInfo * info);
}

// Helper struct to placehold empty parameters
struct RDGParamEmptyPlaceHolder {
    int PlaceHolder;
};

#define BEGIN_SHADER_PARAMETERS(Name) \
class Name { \
private: \
    typedef Name ThisClass; \
    typedef void (*zzFuncPtr)(); \
    struct zzzFirstParam_TypeID {}; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zzzFirstParam_TypeID, \
    [[maybe_unused]] std::vector<RDGShaderParamInfo> * params) { \
        return nullptr; \
    } \
    typedef zzzFirstParam_TypeID

// Declare uniforms, buffers, textures, etc.
#define SHADER_PARAMETER(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    TRDGShaderParamPlaceHolderType<ConstStrHash32(#Type)>::value Name \
     = TRDGShaderParamPlaceHolderType<ConstStrHash32(#Type)>::default_value(); \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, cpp_offset)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// TODO support more blending operations

struct RDGShaderRenderTargetParameter {
    // Keep this as the first member because somewhere in my code may use *(RDGTexture**)(ptr+offset)
    // as a way to access the texture pointer (for legacy reasons).
    RDGTexture * texture {};
    RHILoadOpType load_op {RHILoadOpType::kLoad};
    RHIStoreOpType store_op {RHIStoreOpType::kStore};
    std::array<float, 4> clear_value {0.0f, 0.0f, 0.0f, 1.0f};
    FORCEINLINE operator RDGTexture * () const { return texture; }
    FORCEINLINE RDGShaderRenderTargetParameter & operator = (RDGTexture * tex) {
        texture = tex;
        return *this;
    }
};

// Declare render targets. Can only be used within render pass shader parameter structs.
// Usage: SHADER_RENDER_TARGET(PixelFormat::kR8G8B8A8_UNORM, Name)
#define SHADER_RENDER_TARGET(Format, Name) \
    zz##Name##_PrevTypeID; \
public: \
    RDGShaderRenderTargetParameter Name; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "RenderTarget"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        param_info.cpp_extra.render_targets_info = new RDGShaderRenderTargetInfo {0xffffffffu, Format}; \
        params->emplace_back(param_info); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Specify the renderpass the shader uses.
// Usage: SHADER_USE_RENDERPASS(SomeRenderPassParamStructClass, Name)
#define SHADER_USE_RENDERPASS(PassType, Name) \
    zz##Name##_PrevTypeID; \
public: \
    PassType * Name {}; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "RenderPass"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        auto struct_info = PassType::GetParamStructInfo(); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        assert(struct_info->CanBeRenderpass() && "Renderpass parameter structs should only contain render targets."); \
        params->emplace_back(RDGMakeShaderParamInfo(#PassType, #Name, cpp_offset, struct_info, false, true)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Declare vertex buffers
// Usage: SHADER_VERTEX_BUFFER(16, Name)
#define SHADER_VERTEX_BUFFER(Stride, Name) \
    zz##Name##_PrevTypeID; \
public: \
    RDGBuffer * Name {}; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "VertexBuffer"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        param_info.cpp_extra.vertex_buffer_info = new RDGShaderVertexBufferInfo {0xffffffffu, Stride}; \
        params->emplace_back(param_info); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Declare vertex attribute
// Usage: SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k2xFp32, Name)
#define SHADER_VERTEX_ATTRIBUTE(BufferIndex, Offset, Format, Name) \
    zz##Name##_PrevTypeID; \
private: \
    RDGParamEmptyPlaceHolder zzVertexAttributePlaceHolder_##Name; /* Placeholder - vertex attributes don't have a corresponding member */ \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "VertexAttribute"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, zzVertexAttributePlaceHolder_##Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        param_info.cpp_extra.vertex_attribute_info = new RDGShaderVertexAttributeInfo {BufferIndex, Offset, 0xffffffffu, Format}; \
        params->emplace_back(param_info); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Declare dispatch command
// Usage: SHADER_DISPATCH(Name)
#define SHADER_DISPATCH_COMMAND(Name) \
    zz##Name##_PrevTypeID; \
public: \
    RDGBuffer * Name {}; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "DispatchCommand"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        params->emplace_back(param_info); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID


// Create a nested struct inside the struct.
// The nested struct should not own any shader resources.
#define SHADER_PARAMETER_STRUCT_NESTED(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    Type Name; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        auto struct_info = Type::GetParamStructInfo(); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        assert(struct_info->CanBeImported() && "Parameter structs containing shader resources or other non-nested parameter structs can not be nested."); \
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, cpp_offset, struct_info)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Externally reference a struct. In RHI layer, this will be a unique buffer binding.
// The referenced struct should not own any shader resources.
#define SHADER_PARAMETER_STRUCT_REF(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    Type * Name = reinterpret_cast<Type*>(RDGParameter_UnsetPointer); \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        auto struct_info = Type::GetParamStructInfo(); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        assert(struct_info->CanBeImported() && "Parameter structs containing shader resources or other non-nested parameter structs can not be referenced."); \
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, cpp_offset, struct_info, true)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Include parameters from another struct, and check for name conflicts.
#define SHADER_PARAMETER_STRUCT_INCLUDE(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    Type Name; \
private: \
    struct zz##Name##_TypeID { \
    static constexpr const char * name = #Name; \
    static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        auto struct_info = Type::GetParamStructInfo(); \
        for(auto & e : struct_info->cpp_members) { \
            params->emplace_back(e); \
            params->back().cpp_offset += offsetof(ThisClass, Name); \
        } \
        std::reverse(params->end() - struct_info->cpp_members.size(), params->end()); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

#define END_SHADER_PARAMETERS() \
    zzzLastParam_PrevTypeID; \
    typedef zzFuncPtr (*zzMemberFunc)(zzzFirstParam_TypeID, std::vector<RDGShaderParamInfo> *);\
public: \
    static const RDGShaderParamStructAndSizeInfo * GetParamStructInfo () \
	{ \
        static RDGShaderParamStructAndSizeInfo * params_struct_info_ {}; \
        if (params_struct_info_) return params_struct_info_; \
		std::vector<RDGShaderParamInfo> params; \
		zzFuncPtr (*LastFunc)(zzzLastParam_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
		LastFunc = zz_AppendParamAndGetPrevFuncPtr; \
		zzFuncPtr func_ptr = (zzFuncPtr) LastFunc; \
		do { func_ptr = reinterpret_cast<zzMemberFunc>(func_ptr)(zzzFirstParam_TypeID(), &params); } \
		while (func_ptr); \
		std::reverse(params.begin(), params.end()); \
		bool success = details::zzFinalizeParams(params); \
        if(!success) { \
            MI_LOG(MIInfraLogType::kError, "Failed to finalize shader parameters"); \
            params_struct_info_ = new RDGShaderParamStructAndSizeInfo {}; \
            return params_struct_info_; \
        } \
        auto params_mem = new RDGShaderParamInfo[params.size()]; \
        std::copy(params.begin(), params.end(), params_mem); \
        std::map<uint32_t, int> cpp_member_index_map; \
        for (int i = 0; i < params.size(); i++) { \
            cpp_member_index_map[CRC32String(params[i].name.c_str())] = i; \
        } \
        auto params_span = byte_strided_span((RHIParamInfo*)params_mem, params.size(), sizeof(RDGShaderParamInfo)); \
        auto cpp_params_span = std::span(params_mem, params.size()); \
        params_struct_info_ = new RDGShaderParamStructAndSizeInfo {}; \
        params_struct_info_->members = params_span; \
        params_struct_info_->cpp_members = cpp_params_span; \
        params_struct_info_->cpp_member_index_map = cpp_member_index_map; \
        params_struct_info_->InitializeUniformsLayoutHash(); \
        params_struct_info_->size = params_struct_info_->ComputeSize(); \
        details::zzFinalizeTopLevelParamsStructInfo(params_struct_info_); \
        return params_struct_info_; \
	} \
};

MI_NAMESPACE_END

#endif //RDG_PARAM_H
