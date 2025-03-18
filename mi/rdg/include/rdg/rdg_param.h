/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PARAM_H
#define RDG_PARAM_H

#include <map>
#include <regex>
#include <set>
#include <core/constants.h>

#include "core/crc.h"
#include "core/infra.h"
#include "core/pixel_format.h"
#include "rhi/rhi_param.h"
#include "rdg/rdg_base.h"

MI_NAMESPACE_BEGIN

enum class RDGShaderParamStructImportType {
    // (cpp side) included the entire parameter in the struct memory
    kNested,
    // (cpp side) included a reference to the parameter in the struct memory
    kReference,
    kMax
};

struct RDGShaderParamStructInfo;

struct RDGImportedShaderParamStructInfo {
    const RDGShaderParamStructInfo * cpp_struct_info {nullptr};
    RDGShaderParamStructImportType cpp_import_type {RDGShaderParamStructImportType::kMax};
};

struct RDGShaderRenderTargetInfo {
    uint32_t target_index;
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
        RDGShaderIndexBufferInfo * index_buffer_info;
        // dispatch command does not have extra info.
    } cpp_extra {};
};

// C++ side shader parameter struct + shader side shader parameter struct pair
struct RDGShaderParamStructInfo : public RHIParamStructInfo {
    std::span<RDGShaderParamInfo> cpp_members {};
    // Whether this struct can be imported to another RDGShaderParamStructInfo and wrapped by a RDGImportedShaderParamStructInfo
    // If this struct does not contain any references and shader resources, it can be imported.
    bool CanBeImported () const ;
    std::map<uint32_t /*CRC*/, int> member_index_map;
    FORCEINLINE int GetMemberIndex(uint32_t crc) const {
        auto it = member_index_map.find(crc);
        if(it == member_index_map.end()) return -1;
        return it->second;
    }
    FORCEINLINE int GetMemberIndex (const char * name) const {
        return GetMemberIndex(CRC32String(name));
    }
    FORCEINLINE int GetMemberIndex (std::string_view name) const {
        return GetMemberIndex(CRC32String(name));
    }
};

struct RDGShaderParamStructAndSizeInfo: public RDGShaderParamStructInfo {
    // Cache the device size of the parameter struct at the outer most level
    uint32_t size;
};

// Map hlsl type strings to C++ metadata and types
template<uint32_t CRC> struct TRDGShaderParamPlaceHolderType;
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int")> {
    typedef int value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int2")> {
    typedef glm::ivec2 value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int3")> {
    typedef glm::ivec3 value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("int4")> {
    typedef glm::ivec4 value;
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint")> {
    typedef uint32_t value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint2")> {
    typedef glm::uvec2 value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint3")> {
    typedef glm::uvec2 value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("uint4")> {
    typedef glm::uvec2 value;
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float")> {
    typedef float value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float2")> {
    typedef glm::vec2 value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float3")> {
    typedef glm::vec3 value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("float4")> {
    typedef glm::vec4 value;
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("Texture2D")> {
    typedef RDGTexture * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWTexture2D")> {
    typedef RDGTexture * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("Sampler")> {
    typedef RHISampler * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("Buffer")> {
    typedef RDGBuffer * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("StructuredBuffer")> {
    typedef RDGBuffer * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWBuffer")> {
    typedef RDGBuffer * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RWStructuredBuffer")> {
    typedef RDGBuffer * value;
};

template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("RenderTarget")> {
    typedef RDGTexture * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("VertexBuffer")> {
    typedef RDGBuffer * value;
};
template<> struct TRDGShaderParamPlaceHolderType<ConstStrHash32("IndexBuffer")> {
    typedef RDGBuffer * value;
};

FORCEINLINE RDGShaderParamInfo RDGMakeShaderParamInfo (
    std::string type_name, std::string param_name, uint32_t cpp_offset,
    const RDGShaderParamStructInfo * cpp_struct_info = nullptr, RDGShaderParamStructImportType import_type = RDGShaderParamStructImportType::kNested) {
    RDGShaderParamInfo info {};
    info.name = param_name;
    info.type = RHITypeNameStringToParamType(type_name);
    info.access_flags = TypeNameStringToRHIAccessFlags(type_name);

    if(info.type == RHIParamType::kBasic) {
        info.basic_type = RHITypeNameStringToBasicParamType(type_name);
    }
    if (info.type == RHIParamType::kStruct) {
        info.struct_info = cpp_struct_info;
        info.cpp_imported_struct_info.cpp_struct_info = cpp_struct_info;
        info.cpp_imported_struct_info.cpp_import_type = import_type;
    }
    // info.offset = offset; // Offsets will be assigned when finalizing
    info.cpp_offset = cpp_offset;
    if (info.struct_info) {
        info.size = cpp_struct_info->ComputeSize();
    } else if (info.type == RHIParamType::kBasic) {
        info.size = RHIGetBasicParamSize(info.basic_type);
    }
    return info;
}

namespace details {
    // Compute the device-side uniform buffer size of the parameter struct
    FORCEINLINE bool zzFinalizeParams(std::vector<RDGShaderParamInfo> & params) {
        uint32_t curr_position = 0;
        uint32_t vertex_buffer_index = 0;
        uint32_t vertex_attribute_index = 0;
        uint32_t render_target_index = 0;
        std::map<std::string, size_t> name_to_offset;
        name_to_offset.clear();
        for (auto & e : params) {
            // Ignore non-uniform buffer contents (shader resources, uniform buffer ref)
            if (e.type == RHIParamType::kBasic || e.type == RHIParamType::kStruct) {
                auto alignment = e.GetAlignment();
                curr_position = (curr_position + alignment - 1) & ~(alignment - 1);
                // HLSL buffer-row rule check: if the element lies on the 16-byte boundary, it should be aligned to 16 bytes
                if (e.type == RHIParamType::kBasic) {
                    auto param_size = RHIGetBasicParamSize(e.basic_type);
                    auto start_row = curr_position / 16;
                    auto end_row = (curr_position + param_size - 1) / 16;
                    if (start_row != end_row) {
                        curr_position = (curr_position + 16 - 1) & ~(16 - 1);
                    }
                }
                e.offset = curr_position;
                curr_position += e.size;
                auto it = name_to_offset.find(e.name);
                if (it != name_to_offset.end()) {
                    // FIXME compile error, why?
                    assert(false);
                    // MI_LOG(MIInfraLogType::kError, "Duplicate param name: {}", e.name);
                    return false;
                }
                name_to_offset[e.name] = e.offset;
            }
            if (e.type == RHIParamType::kVertexBuffer) {
                // Finalize vertex buffer index now
                e.cpp_extra.vertex_buffer_info->index = vertex_buffer_index++;
            }
            if (e.type == RHIParamType::kVertexAttribute) {
                // Finalize vertex attribute index now
                e.cpp_extra.vertex_attribute_info->attribute_index = vertex_attribute_index++;
            }
            if (e.type == RHIParamType::kRenderTarget) {
                // Finalize render target index now
                e.cpp_extra.render_targets_info->target_index = render_target_index++;
            }
        }
        // Do some simple validation
        bool index_present = false;
        for (auto & e : params) {
            if (e.type == RHIParamType::kVertexAttribute) {
                if (e.cpp_extra.vertex_attribute_info->buffer_index >= vertex_attribute_index) {
                    MI_LOG(MIInfraLogType::kError,
                        "Invalid vertex attribute buffer index (provided {}, {} is out of range, attribute name {}).",
                        e.cpp_extra.vertex_attribute_info->buffer_index, vertex_attribute_index, e.name);
                    return false;
                }
            }
            if (e.type == RHIParamType::kIndexBuffer) {
                if (!index_present) index_present = true;
                else {
                    MI_LOG(MIInfraLogType::kError,
                        "More than one index buffer is provided. (provided {}).", e.name
                    );
                }
            }
        }
        return true;
    }
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
    TRDGShaderParamPlaceHolderType<ConstStrHash32(#Type)>::value Name; \
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

// Declare render targets
// Usage: SHADER_RENDER_TARGET(PixelFormat::kR8G8B8A8_UNORM, Name)
#define SHADER_RENDER_TARGET(Format, Name) \
    zz##Name##_PrevTypeID; \
public: \
    RDGTexture * Name; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "RenderTarget"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        param_info->cpp_extra.render_targets_info = new RDGShaderRenderTargetInfo {0xffffffffu, Format}; \
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
    RDGBuffer * Name; \
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
public: \
    RDGParamEmptyPlaceHolder Name; /* Placeholder - vertex attributes don't have a corresponding member */ \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "VertexAttribute"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        param_info.cpp_extra.vertex_attribute_info = new RDGShaderVertexAttributeInfo {BufferIndex, Offset, 0xffffffffu, Format}; \
        params->emplace_back(param_info); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Declare index buffer
// Usage: SHADER_INDEX_BUFFER(RHIIndexType::kUint32, Name)
#define SHADER_INDEX_BUFFER(Format, Name) \
    zz##Name##_PrevTypeID; \
public: \
    RDGBuffer * Name; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = "IndexBuffer"; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t cpp_offset = offsetof(ThisClass, Name); \
        auto param_info = RDGMakeShaderParamInfo(zz##Name##_TypeID::type_name, #Name, cpp_offset); \
        param_info.cpp_extra.index_buffer_info = new RDGShaderIndexBufferInfo {Format}; \
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
    RDGBuffer * Name; \
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
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, cpp_offset, struct_info, RDGShaderParamStructImportType::kNested)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Externally reference a struct. In RHI layer, this will be a unique buffer binding.
// The referenced struct should not own any shader resources.
#define SHADER_PARAMETER_STRUCT_REF(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    Type * Name; \
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
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, cpp_offset, struct_info, RDGShaderParamStructImportType::kReference)); \
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
        std::map<uint32_t, int> member_index_map; \
        for (int i = 0; i < params.size(); i++) { \
            member_index_map[CRC32String(params[i].name.c_str())] = i; \
        } \
        auto params_span = byte_strided_span((RHIParamInfo*)params_mem, params.size(), sizeof(RDGShaderParamInfo)); \
        auto cpp_params_span = std::span(params_mem, params.size()); \
        params_struct_info_ = new RDGShaderParamStructAndSizeInfo {}; \
        params_struct_info_->members = params_span; \
        params_struct_info_->cpp_members = cpp_params_span; \
        params_struct_info_->member_index_map = member_index_map; \
        params_struct_info_->InitializeLayoutHash(); \
        params_struct_info_->size = params_struct_info_->ComputeSize(); \
        return params_struct_info_; \
	} \
};

MI_NAMESPACE_END

#endif //RDG_PARAM_H
