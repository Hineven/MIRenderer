/*
 * Created: 2025/3/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_PARAM_H
#define RHI_PARAM_H

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <iterator>
#include <core/crc.h>
#include <glm/glm.hpp>

#include "core/util/byte_strided_span.h"
#include "rhi/rhi_common.h"

MI_NAMESPACE_BEGIN

enum class RHIParamType {
    kUAVBuffer,
    kSRVBuffer,
    kUAVTexture,
    kSRVTexture,
    kSampler,
    // kConstantBuffer, // Uniform buffer
    kStruct,
    kBasic,
    kMax,
};

enum class RHIBasicParamType {
    kFloat,
    kFloat2,
    kFloat3,
    kFloat4,
    kInt,
    kInt2,
    kInt3,
    kInt4,
    kUInt,
    kUInt2,
    kUInt3,
    kUInt4,
    kMax
};

FORCEINLINE RHIBasicParamType StringToRHIBasicParamType (std::string_view type) {
    if(type == "float") return RHIBasicParamType::kFloat;
    if(type == "float2") return RHIBasicParamType::kFloat2;
    if(type == "float3") return RHIBasicParamType::kFloat3;
    if(type == "float4") return RHIBasicParamType::kFloat4;
    if(type == "int") return RHIBasicParamType::kInt;
    if(type == "int2") return RHIBasicParamType::kInt2;
    if(type == "int3") return RHIBasicParamType::kInt3;
    if(type == "int4") return RHIBasicParamType::kInt4;
    if(type == "uint") return RHIBasicParamType::kUInt;
    if(type == "uint2") return RHIBasicParamType::kUInt2;
    if(type == "uint3") return RHIBasicParamType::kUInt3;
    if(type == "uint4") return RHIBasicParamType::kUInt4;
    return RHIBasicParamType::kMax;
}

FORCEINLINE RHIParamType StringToRHIParamType (std::string_view type) {
    if(type == "Texture2D") return RHIParamType::kSRVTexture;
    if(type == "RWTexture2D") return RHIParamType::kUAVTexture;
    if(type == "Sampler") return RHIParamType::kSampler;
    if(type == "Buffer") return RHIParamType::kSRVBuffer;
    if(type == "StructuredBuffer") return RHIParamType::kSRVBuffer;
    if(type == "RWBuffer") return RHIParamType::kUAVBuffer;
    if(type == "RWStructuredBuffer") return RHIParamType::kUAVBuffer;
    if(type == "ConstantBuffer") return RHIParamType::kStruct;
    if(StringToRHIBasicParamType(type) != RHIBasicParamType::kMax) return RHIParamType::kBasic;
    return RHIParamType::kStruct;
}

FORCEINLINE uint32_t RHIGetBasicParamSize (RHIBasicParamType type) {
    switch (type) {
        case RHIBasicParamType::kFloat: return sizeof(float);
        case RHIBasicParamType::kFloat2: return sizeof(glm::vec2);
        case RHIBasicParamType::kFloat3: return sizeof(glm::vec3);
        case RHIBasicParamType::kFloat4: return sizeof(glm::vec4);
        case RHIBasicParamType::kInt: return sizeof(int);
        case RHIBasicParamType::kInt2: return sizeof(glm::ivec2);
        case RHIBasicParamType::kInt3: return sizeof(glm::ivec3);
        case RHIBasicParamType::kInt4: return sizeof(glm::ivec4);
        case RHIBasicParamType::kUInt: return sizeof(uint32_t);
        case RHIBasicParamType::kUInt2: return sizeof(glm::uvec2);
        case RHIBasicParamType::kUInt3: return sizeof(glm::uvec3);
        case RHIBasicParamType::kUInt4: return sizeof(glm::uvec4);
        default: return 0;
    }
}

// We are using the (glsl) scalar layout, so the rules largely matches C structs
// https://maraneshi.github.io/HLSL-ConstantBufferLayoutVisualizer/
FORCEINLINE uint32_t RHIGetBasicParamAlignment (RHIBasicParamType type) {
    switch (type) {
        case RHIBasicParamType::kFloat: return sizeof(float);
        case RHIBasicParamType::kFloat2: return sizeof(float);
        case RHIBasicParamType::kFloat3: return sizeof(float);
        case RHIBasicParamType::kFloat4: return sizeof(glm::vec4);
        case RHIBasicParamType::kInt: return sizeof(int);
        case RHIBasicParamType::kInt2: return sizeof(int);
        case RHIBasicParamType::kInt3: return sizeof(int);
        case RHIBasicParamType::kInt4: return sizeof(glm::ivec4);
        case RHIBasicParamType::kUInt: return sizeof(uint32_t);
        case RHIBasicParamType::kUInt2: return sizeof(uint32_t);
        case RHIBasicParamType::kUInt3: return sizeof(uint32_t);
        case RHIBasicParamType::kUInt4: return sizeof(glm::uvec4);
        default: return 0;
    }
}

struct RHIParamStructInfo ;

struct RHIParamInfo {
    std::string name;
    RHIParamType type;
    RHIBasicParamType basic_type;
    // Reflection valid for StructuredBuffer, RWStructuredBuffer, ConstantBuffer
    const RHIParamStructInfo * struct_info;
    uint32_t offset; // Only makes sense for members inside a struct
    uint32_t size;
    // uint32_t array_size; // Arrays not supported currently
    uint32_t GetAlignment () const ;
};

struct RHIParamStructInfo {
    byte_strided_span<RHIParamInfo> members;
    uint32_t layout_hash;
    FORCEINLINE uint32_t GetSize () const {
        uint32_t curr_position = 0;
        for (auto & e : members) {
            auto alignment = e.GetAlignment();
            curr_position = (curr_position + alignment - 1) & ~(alignment - 1);
            curr_position += e.size;
        }
        return curr_position;
    }
    void InitializeLayoutHash ();
};

MI_NAMESPACE_END

#endif //RHI_PARAM_H
