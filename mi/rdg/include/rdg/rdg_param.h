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
#include "rhi/rhi_desc.h"
#include "rhi/rhi_fwd.h"
#include "rdg/rdg_base.h"
#include "rdg/rdg_global_memory_collector.h"

MI_NAMESPACE_BEGIN

struct RDGShaderParamStructAndSizeInfo;

struct RDGImportedShaderParamStructInfo {
    const RDGShaderParamStructAndSizeInfo * cpp_struct_info {nullptr};
};
struct RDGShaderRenderTargetBlendingSettings {
    RHIBlendOpType blend_op {RHIBlendOpType::kMax}; // kMax for no blending
    RHIBlendFactorType src_blend {RHIBlendFactorType::kSrcAlpha};
    RHIBlendFactorType dst_blend {RHIBlendFactorType::kOneMinusSrcAlpha};
};
struct RDGShaderRenderTargetInfo {
    uint32_t target_index; // UINT32_MAX for depth stencil
    PixelFormatType format;
    RDGShaderRenderTargetBlendingSettings blending {};
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
    // Whether this struct describes a renderpass. A renderpass should only be described via a series of render targets.
    bool CanBeRenderpass () const;
    std::map<uint32_t /*name CRC*/, int> cpp_member_index_map;
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
    uint32_t size;
};

struct RDGShaderSignatureParamInfo : public RDGShaderParamStructInfo {
    std::span<RDGShaderParameterLocation> storage_buffers_;
    std::span<RDGShaderParameterLocation> uniform_buffers_;
    std::span<RDGShaderParameterLocation> uavs_;
    std::span<RDGShaderParameterLocation> srvs_;
    std::span<RDGShaderParameterLocation> samplers_;
    std::span<RDGShaderParameterLocation> acceleration_structures_;
};

struct RDGShaderRenderPassInfo {
    std::span<RDGShaderParameterLocation> vertex_buffers_;
    std::span<RDGShaderParameterLocation> vertex_attributes_;
    RDGShaderParameterLocation index_buffer_;
    std::span<RDGShaderParameterLocation> render_targets_;
};

struct RDGShaderParamStructAndSizeInfo : public RDGShaderSignatureParamInfo {
    RDGShaderRenderPassInfo render_pass_info_;
};

// Map type strings to C++ metadata and types
template<uint32_t CRC> struct TRDGShaderParamPlaceHolderType;

struct RDGShaderTextureParameter {
    // Keep this as the first member because somewhere in my code may use *(RDGTexture**)(ptr+offset)
    // as a way to access the texture pointer (for legacy reasons).
    RDGTexture * texture {};
    // The layer of the texture to bind to. This is used for 3D textures and cube maps.
    // Leave UINT_MAX for whole texture arrays / defaults. And for Texture2DArray and TextureCube, this has to be UINT_MAX.
    uint32_t array_layer {UINT_MAX};
    // The mip level of the texture to bind to. Defaults to 0. Makes no sense when the texture is only sampled.
    uint32_t mip_level {0};

    FORCEINLINE RDGShaderTextureParameter(RDGTexture * tex) : texture(tex) {}

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

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWTexture2DArray")>
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

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("AccelerationStructure")> {
    typedef RHIAccelerationStructure * value;
    FORCEINLINE static RHIAccelerationStructure * default_value() {
        return reinterpret_cast<RHIAccelerationStructure*>(RDGParameter_UnsetPointer);
    }
};

FORCEINLINE RDGShaderParamInfo RDGMakeShaderParamInfo (
    const std::string& type_name, std::string param_name, uint32_t size, uint32_t cpp_offset
) {
    RDGShaderParamInfo info {};
    info.name = std::move(param_name);
    info.type = RHITypeNameStringToParamType(type_name);
    if (type_name == "VertexBuffer") {
        info.access_flags = RHIGPUAccessFlagBits::kVertexAttributeRead;
    } else if (type_name == "RenderTarget" || type_name == "VertexAttribute") {
        // RenderTarget is a special type, it has no access flags.
    } else info.access_flags = TypeNameStringToRHIAccessFlags(type_name);
    if (info.type == RHIParamType::kUniformBuffer) {
        info.size = size;
    } else info.size = 0;
    info.cpp_offset = cpp_offset;
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

// Declare buffers, textures, etc.
// Example: SHADER_RESOURCE_PARAMETER(RWTexture2D, TextureName) -> UAV 2D texture declared as TextureName
// Example: SHADER_RESOURCE_PARAMETER(Texture2DArray, TextureName) -> 2D texture array declared as TextureName
// Example: SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, BufferName)
// Example: SHADER_RESOURCE_PARAMETER(AccelerationStructure, ASName)
#define SHADER_RESOURCE_PARAMETER(Type, Name) \
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
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, 0, cpp_offset)); \
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
    // Destination of the array layer of the render target. In case the texture is a texture array or a cube map.
    uint32_t array_layer {0};
    // Can be interpreted as anything. For example, for an uint render target, use std::bit_cast<float>(0xFFFFFFFFu) for clear value of 0xFFFFFFFFu.
    std::array<float, 4> clear_value {0.0f, 0.0f, 0.0f, 1.0f};
    FORCEINLINE operator RDGTexture * () const { return texture; }
    FORCEINLINE RDGShaderRenderTargetParameter & operator = (RDGTexture * tex) {
        texture = tex;
        return *this;
    }
};

// Declare render targets. Can only be used within render pass shader parameter structs.
// Usage: SHADER_RENDER_TARGET(PixelFormat::kR8G8B8A8_UNORM, Name)
// Append an optional initializer for RDGShaderRenderTargetBlendingSettings after the name param if advanced
// blending settings are needed.
#define SHADER_RENDER_TARGET(Format, Name, ...) \
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
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, 0, cpp_offset); \
        auto __p = RDGGlobalMemoryCollector::Get().New<RDGShaderRenderTargetInfo>(); \
        __p->target_index = 0xffffffffu; \
        __p->format = Format; \
        __VA_OPT__( __p->blending = __VA_ARGS__ ; ) \
        param_info.cpp_extra.render_targets_info = __p; \
        params->emplace_back(param_info); \
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
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, 0, cpp_offset); \
        auto __p = RDGGlobalMemoryCollector::Get().New<RDGShaderVertexBufferInfo>(); \
        __p->index = 0xffffffffu; \
        __p->stride = Stride; \
        param_info.cpp_extra.vertex_buffer_info = __p; \
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
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, 0, cpp_offset); \
        auto __p = RDGGlobalMemoryCollector::Get().New<RDGShaderVertexAttributeInfo>(); \
        __p->buffer_index = BufferIndex; \
        __p->offset = Offset; \
        __p->attribute_index = 0xffffffffu; \
        __p->format = Format; \
        param_info.cpp_extra.vertex_attribute_info = __p; \
        params->emplace_back(param_info); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Add a uniform buffer. Corresponding HLSL: ConstantBuffer<Type> Name;
#define SHADER_UNIFORM_BUFFER(Type, Name) \
    zz##Name##_PrevTypeID; \
    static_assert(CMemTrivial<Type>, "Uniform struct type must be CMemTrivial"); \
public: \
    Type * Name = reinterpret_cast<Type*>(RDGParameter_UnsetPointer); \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        params->emplace_back(RDGMakeShaderParamInfo("ConstantBuffer", #Name, (uint32_t)sizeof(Type), cpp_offset)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

#define END_SHADER_PARAMETERS() \
    zzzLastParam_PrevTypeID; \
    typedef zzFuncPtr (*zzMemberFunc)(zzzFirstParam_TypeID, std::vector<RDGShaderParamInfo> *);\
protected: \
    static RDGShaderParamStructAndSizeInfo * params_struct_info_; \
public: \
    static void InitParamStructInfo() { \
        if(params_struct_info_) return ; \
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
            params_struct_info_ = RDGGlobalMemoryCollector::Get().New<RDGShaderParamStructAndSizeInfo>(); \
            return ; \
        } \
        auto params_mem = RDGGlobalMemoryCollector::Get().NewArray<RDGShaderParamInfo>(params.size()); \
        std::copy(params.begin(), params.end(), params_mem); \
        std::map<uint32_t, int> cpp_member_index_map; \
        for (int i = 0; i < params.size(); i++) { \
            cpp_member_index_map[CRC32String(params[i].name.c_str())] = i; \
        } \
        auto params_span = byte_strided_span((RHIParamInfo*)params_mem, params.size(), sizeof(RDGShaderParamInfo)); \
        auto cpp_params_span = std::span(params_mem, params.size()); \
        params_struct_info_ = RDGGlobalMemoryCollector::Get().New<RDGShaderParamStructAndSizeInfo>(); \
        params_struct_info_->members = params_span; \
        params_struct_info_->cpp_members = cpp_params_span; \
        params_struct_info_->cpp_member_index_map = cpp_member_index_map; \
        details::zzFinalizeTopLevelParamsStructInfo(params_struct_info_); \
    } \
    static const RDGShaderParamStructAndSizeInfo * GetParamStructInfo () \
	{ \
        return params_struct_info_; \
	} \
};

// Only externally defined shader parameter structs (that may be shared among multiple shaders)
// requires this macro for implementing its static class members.
#define IMPLEMENT_SHADER_PARAMETERS(Name) \
    RDGShaderParamStructAndSizeInfo * Name::params_struct_info_; \

MI_NAMESPACE_END

#endif //RDG_PARAM_H
