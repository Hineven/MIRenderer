/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PARAM_H
#define RDG_PARAM_H

#include <map>
#include "core/crc.h"
#include "rhi/rhi_param.h"
#include "rdg/rdg_base.h"

MI_NAMESPACE_BEGIN

struct RDGShaderParamStructInfo;

enum class RDGShaderParamType {
    kDefault,
    kRef // Reference to an externally created struct, cpp is a pointer
};

struct RDGShaderParamInfo : public RHIParamInfo {
    const RDGShaderParamStructInfo * cpp_struct_info;
    RDGShaderParamType cpp_type;
    // Record the offset of the class member in host memory (cpu side shader parameter struct)
    uint32_t cpp_offset;
};

struct RDGShaderParamStructInfo : public RHIParamStructInfo {
    std::span<RDGShaderParamInfo> cpp_members;
    std::map<uint32_t /*CRC*/, int> member_index_map;
    FORCEINLINE int GetMemberIndex(uint32_t crc) const {
        auto it = member_index_map.find(crc);
        if(it == member_index_map.end()) return -1;
        return it->second;
    }
    FORCEINLINE int GetMemberIndex (const char * name) const {
        return GetMemberIndex(CRC32(name));
    }
};

// Map hlsl type strings to C++ metadata and types
template<uint32_t CRC> struct TRDGParamType;
template<> struct TRDGParamType<ConstStrHash32("int")> {
    typedef int PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kInt;
};
template<> struct TRDGParamType<ConstStrHash32("int2")> {
    typedef glm::ivec2 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kInt2;
};
template<> struct TRDGParamType<ConstStrHash32("int3")> {
    typedef glm::ivec3 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kInt3;
};
template<> struct TRDGParamType<ConstStrHash32("int4")> {
    typedef glm::ivec4 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kInt4;
};

template<> struct TRDGParamType<ConstStrHash32("uint")> {
    typedef uint32_t PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kUInt;
};
template<> struct TRDGParamType<ConstStrHash32("uint2")> {
    typedef glm::uvec2 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kUInt2;
};
template<> struct TRDGParamType<ConstStrHash32("uint3")> {
    typedef glm::uvec2 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kUInt3;
};
template<> struct TRDGParamType<ConstStrHash32("uint4")> {
    typedef glm::uvec2 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kUInt4;
};

template<> struct TRDGParamType<ConstStrHash32("float")> {
    typedef float PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kFloat;
};
template<> struct TRDGParamType<ConstStrHash32("float2")> {
    typedef glm::vec2 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kFloat2;
};
template<> struct TRDGParamType<ConstStrHash32("float3")> {
    typedef glm::vec3 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kFloat3;
};
template<> struct TRDGParamType<ConstStrHash32("float4")> {
    typedef glm::vec4 PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kBasic;
    static constexpr RHIBasicParamType BasicParamType = RHIBasicParamType::kFloat4;
};

template<> struct TRDGParamType<ConstStrHash32("Texture2D")> {
    typedef RDGTexture * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kSRVTexture;
};
template<> struct TRDGParamType<ConstStrHash32("RWTexture2D")> {
    typedef RDGTexture * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kUAVTexture;
};
template<> struct TRDGParamType<ConstStrHash32("Sampler")> {
    typedef RHISampler * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kSampler;
};
template<> struct TRDGParamType<ConstStrHash32("Buffer")> {
    typedef RDGBuffer * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kSRVBuffer;
};
template<> struct TRDGParamType<ConstStrHash32("StructuredBuffer")> {
    typedef RDGBuffer * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kSRVBuffer;
};
template<> struct TRDGParamType<ConstStrHash32("RWBuffer")> {
    typedef RDGBuffer * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kUAVBuffer;
};
template<> struct TRDGParamType<ConstStrHash32("RWStructuredBuffer")> {
    typedef RDGBuffer * PlaceHolderType;
    static constexpr RHIParamType ParamType = RHIParamType::kUAVBuffer;
};

FORCEINLINE RDGShaderParamInfo RDGMakeShaderParamInfo (
    std::string type_name, std::string param_name, uint32_t cpp_offset, RDGShaderParamType cpp_type,
    const RDGShaderParamStructInfo * cpp_struct_info = nullptr) {
    RDGShaderParamInfo info {};
    info.name = param_name;
    info.type = StringToRHIParamType(type_name);
    info.cpp_type = cpp_type;
    if(info.type == RHIParamType::kBasic) {
        info.basic_type = StringToRHIBasicParamType(type_name);
    }
    if (info.type == RHIParamType::kStruct || info.type == RHIParamType::kConstantBuffer) {

        info.struct_info = cpp_struct_info;
        info.cpp_struct_info = cpp_struct_info;
    }
    // info.offset = offset; // Offsets will be assigned when finalizing
    info.cpp_offset = cpp_offset;
    if (info.struct_info) {
        info.size = cpp_struct_info->GetSize();
    } else if (info.type == RHIParamType::kBasic) {
        info.size = RHIGetBasicParamSize(info.basic_type);
    }
    return info;
}

namespace details {
    FORCEINLINE bool zzFinalizeParams(std::vector<RDGShaderParamInfo> & params) {
        uint32_t curr_position = 0;
        std::map<std::string, size_t> name_to_offset;
        name_to_offset.clear();
        for (auto & e : params) {
            auto alignment = e.GetAlignment();
            curr_position = (curr_position + alignment - 1) & ~(alignment - 1);
            // Buffer-row rule check: if the element lies on the 16-byte boundary, it should be aligned to 16 bytes
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
        return true;
    }
}

#define BEGIN_SHADER_PARAMETERS(Name) \
class Name { \
private: \
    typedef Name ThisClass; \
    typedef void (*zzFuncPtr)(); \
    struct zzzFirstParam_TypeID {}; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zzzFirstParam_TypeID, [[maybe_unused]] std::vector<RDGShaderParamInfo> * params) { \
        return nullptr; \
    } \
    typedef zzzFirstParam_TypeID

#define SHADER_PARAMETER(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    TRDGParamType<ConstStrHash32(#Type)>::PlaceHolderType Name; \
private: \
    struct zz##Name##_TypeID { \
        static constexpr const char * name = #Name; \
        static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RDGShaderParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RDGShaderParamInfo> *); \
        uint32_t offset = offsetof(ThisClass, Name); \
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, offset, RDGShaderParamType::kDefault)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

#define SHADER_PARAMETER_STRUCT(Type, Name) \
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
        auto struct_info = &Type::GetParamsMetaData(); \
        uint32_t offset = offsetof(ThisClass, Name); \
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, offset, RDGShaderParamType::kDefault, struct_info)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Use a separate uniform buffer for the struct
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
        auto struct_info = &Type::GetParamsMetaData(); \
        uint32_t offset = offsetof(ThisClass, Name); \
        params->emplace_back(RDGMakeShaderParamInfo(#Type, #Name, offset, RDGShaderParamType::kRef, struct_info)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Include parameters from another struct
#define SHADER_PARAMETER_INCLUDE(Type, Name) \
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
        auto struct_info = &Type::GetParamsMetaData(); \
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
    static const RDGShaderParamStructInfo & GetParamsMetaData() \
	{ \
        static RDGShaderParamStructInfo * zz_params_meta_ {}; \
        if (zz_params_meta_) return * zz_params_meta_; \
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
            zz_params_meta_ = new RDGShaderParamStructInfo {}; \
            return * zz_params_meta_; \
        } \
        auto params_mem = new RDGShaderParamInfo[params.size()]; \
        std::copy(params.begin(), params.end(), params_mem); \
        std::map<uint32_t, int> member_index_map; \
        for (int i = 0; i < params.size(); i++) { \
            member_index_map[CRC32(params[i].name.c_str())] = i; \
        } \
        auto params_span = byte_strided_span((RHIParamInfo*)params_mem, params.size(), sizeof(RDGShaderParamInfo)); \
        auto cpp_params_span = std::span(params_mem, params.size()); \
        zz_params_meta_ = new RDGShaderParamStructInfo {params_span, cpp_params_span}; \
        zz_params_meta_->member_index_map = member_index_map; \
        return * zz_params_meta_; \
	} \
};

MI_NAMESPACE_END

#endif //RDG_PARAM_H
