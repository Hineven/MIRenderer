/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PARAM_H
#define RDG_PARAM_H

#include "core/crc.h"
#include "rhi/rhi_param.h"
#include "rdg/rdg_base.h"

MI_NAMESPACE_BEGIN

struct RDGShaderParamMetaData : public RHIParamInfo {
    RDGShaderParamMetaData * struct;asdasds
    uint32_t cpp_offset;
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

FORCEINLINE RDGShaderParamMetaData RHIMakeShaderParamMetaData (std::string type_name, std::string param_name, const RHIParamStructInfo * struct_info = nullptr) {
    RHIParamInfo info {};
    info.name = param_name;
    info.type = StringToRHIParamType(type_name);
    if(info.type == RHIParamType::kBasic) {
        info.basic_type = StringToRHIBasicParamType(type_name);
    }
    if (info.type == RHIParamType::kStruct || info.type == RHIParamType::kConstantBuffer) {
        info.struct_info = struct_info;
    }
    // info.offset = offset; // Will be assigned when finalizing
    if (info.struct_info) {
        info.size = struct_info->GetSize();
    } else if (info.type == RHIParamType::kBasic) {
        info.size = RHIGetBasicParamSize(info.basic_type);
    }
    return info;
}

namespace details {
    FORCEINLINE bool zzFinalizeParams(std::vector<RHIParamInfo> & params) {
        uint32_t curr_position = 0;
        std::map<std::string, size_t> name_to_offset;
        name_to_offset.clear();
        for (auto & e : params) {
            auto alignment = e.GetAlignment();
            curr_position = (curr_position + alignment - 1) & ~(alignment - 1);
            e.offset = curr_position;
            curr_position += e.size;
            auto it = name_to_offset.find(e.name);
            if (it != name_to_offset.end()) {
                MI_LOG(MIInfraLogType::kError, "Duplicate param name: {}", e.name);
                return false;
            }
            name_to_offset[e.name] = e.offset;
        }
    }
}

#define BEGIN_SHADER_PARAMETERS(Name) \
class Name { \
private: \
    typedef void (*zzFuncPtr)(); \
    struct zzzFirstParam_TypeID {}; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zzzFirstParam_TypeID, [[maybe_unused]] std::vector<RHIParamInfo> * params) { \
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
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RHIParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RHIParamInfo> *); \
        params->emplace_back(RHIMakeShaderParamMetaData(#Type, #Name)); \
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
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RHIParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RHIParamInfo> *); \
        auto struct_info = &Type::GetParamsMetaData(); \
        params->emplace_back(RHIMakeShaderParamMetaData(#Type, #Name, struct_info)); \
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
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RHIParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RHIParamInfo> *); \
        auto struct_info = &Type::GetParamsMetaData(); \
        params->emplace_back(RHIMakeShaderParamMetaData(#Type, #Name, struct_info)); \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

// Include parameters from another struct
#define SHADER_PARAMETER_INCLUDE(Type, Name) \
    zz##Name##_PrevTypeID; \
public: \
    Type * Name; \
private: \
    struct zz##Name##_TypeID { \
    static constexpr const char * name = #Name; \
    static constexpr const char * type_name = #Type; \
    }; \
    static zzFuncPtr zz_AppendParamAndGetPrevFuncPtr(zz##Name##_TypeID, std::vector<RHIParamInfo> * params) { \
        zzFuncPtr (*PrevFunc)(zz##Name##_PrevTypeID, std::vector<RHIParamInfo> *); \
        auto struct_info = &Type::GetParamsMetaData(); \
        for(auto & e : struct_info->members) { \
            params->emplace_back(e); \
        } \
        PrevFunc = zz_AppendParamAndGetPrevFuncPtr; \
        return (zzFuncPtr)PrevFunc; \
    } \
    typedef zz##Name##_TypeID

#define END_SHADER_PARAMETERS() \
    zzzLastParam_PrevTypeID; \
    typedef zzFuncPtr (*zzMemberFunc)(zzzFirstParam_TypeID, std::vector<RHIParamInfo> *);\
public: \
    static const RHIParamStructInfo & GetParamsMetaData() \
	{ \
        static RHIParamStructInfo * zz_params_meta_ {}; \
        static std::map<std::string, size_t> name_to_cpp_offset; \
        if (zz_params_meta_) return * zz_params_meta_; \
		std::vector<RHIParamInfo> params; \
		zzFuncPtr (*LastFunc)(zzzLastParam_PrevTypeID, std::vector<RHIParamInfo> *); \
		LastFunc = zz_AppendParamAndGetPrevFuncPtr; \
		zzFuncPtr func_ptr = (zzFuncPtr) LastFunc; \
		do { func_ptr = reinterpret_cast<zzMemberFunc>(func_ptr)(zzzFirstParam_TypeID(), &params); } \
		while (func_ptr); \
		std::reverse(params.begin(), params.end()); \
		bool success = details::zzFinalizeParams(params); \
        if(!success) { \
            MI_LOG(MIInfraLogType::kError, "Failed to finalize shader parameters"); \
            zz_params_meta_ = new RHIParamStructInfo {}; \
            return * zz_params_meta_; \
        } \
        zz_params_meta_ = new RHIParamStructInfo {params}; \
        return * zz_params_meta_; \
	} \
};

MI_NAMESPACE_END

#endif //RDG_PARAM_H
