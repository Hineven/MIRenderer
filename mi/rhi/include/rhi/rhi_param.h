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

#include "rhi_types.h"
#include "core/util/byte_strided_span.h"

MI_NAMESPACE_BEGIN

enum class RHIParamType : uint32_t {
    kStorageBuffer = 0,
    kUniformBuffer,
    kUAVTexture,
    kSRVTexture,
    kSampler,
    kStruct,
    kBasic,
    kAccelerationStructure,
    // The following 6 types have no mapping in hlsl, just corporate with RDG shader reflection.
    kRenderTarget,
    kVertexAttribute,
    kVertexBuffer,
    kIndexBuffer,
    kDispatchCommand,
    kRenderPass,
    kMax,
};

FORCEINLINE std::string ToString (RHIParamType type) {
    switch (type) {
        case RHIParamType::kStorageBuffer: return "StorageBuffer";
        case RHIParamType::kUniformBuffer: return "UniformBuffer";
        case RHIParamType::kUAVTexture: return "UAVTexture";
        case RHIParamType::kSRVTexture: return "SRVTexture";
        case RHIParamType::kSampler: return "Sampler";
        case RHIParamType::kStruct: return "Struct";
        case RHIParamType::kBasic: return "Basic";
        case RHIParamType::kAccelerationStructure: return "AccelerationStructure";
        case RHIParamType::kRenderTarget: return "RenderTarget";
        case RHIParamType::kVertexAttribute: return "VertexAttribute";
        case RHIParamType::kVertexBuffer: return "VertexBuffer";
        case RHIParamType::kIndexBuffer: return "IndexBuffer";
        case RHIParamType::kDispatchCommand: return "DispatchCommand";
        case RHIParamType::kMax: return "Max";
        default: return "Unknown";
    }
}

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

FORCEINLINE RHIBasicParamType RHITypeNameStringToBasicParamType (std::string_view type) {
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

FORCEINLINE RHIParamType RHITypeNameStringToParamType (std::string_view type) {
    if(type == "Texture2D") return RHIParamType::kSRVTexture;
    if(type == "RWTexture2D") return RHIParamType::kUAVTexture;
    if(type == "SamplerState") return RHIParamType::kSampler;
    if(type == "Buffer") return RHIParamType::kStorageBuffer;
    if(type == "StructuredBuffer") return RHIParamType::kStorageBuffer;
    if(type == "RWBuffer") return RHIParamType::kStorageBuffer;
    if(type == "RWStructuredBuffer") return RHIParamType::kStorageBuffer;
    if(type == "ConstantBuffer") return RHIParamType::kUniformBuffer;
    if(type == "AccelerationStructure") return RHIParamType::kAccelerationStructure;
    // The following 4 types have no mapping in hlsl, just corporate with RDG shader reflection.
    if(type == "RenderTarget") return RHIParamType::kRenderTarget;
    if(type == "VertexAttribute") return RHIParamType::kVertexAttribute;
    if(type == "VertexBuffer") return RHIParamType::kVertexBuffer;
    if(type == "IndexBuffer") return RHIParamType::kIndexBuffer;
    if(type == "DispatchCommand") return RHIParamType::kDispatchCommand;
    if(RHITypeNameStringToBasicParamType(type) != RHIBasicParamType::kMax) return RHIParamType::kBasic;
    return RHIParamType::kStruct;
}

FORCEINLINE RHIGPUAccessFlags TypeNameStringToRHIAccessFlags (std::string_view type) {
    if (type.length() >= 2 && type.starts_with("RW")) {
        return RHIGPUAccessFlagBits::kAll;
    }
    return RHIGPUAccessFlagBits::kRead;
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
    // Currently only meaningful for storage buffers. Otherwise, it can be any value.
    RHIGPUAccessFlags access_flags;
    // Device side (shader) memory offset within the parent struct.
    uint32_t offset; // Only makes sense for members inside a struct
    uint32_t size;
    // uint32_t array_size; // Arrays not supported currently
    uint32_t GetAlignment () const ;
};

struct RHIParamStructInfo {
    // Layout hash only considering uniforms and their relative orders.
    uint32_t uniforms_layout_hash;
    byte_strided_span<RHIParamInfo> members;
    void InitializeUniformsLayoutHash ();
    // Compute device-side uniform buffer size for this struct.
    // Note: we'll omit shader resource members (such as textures) that can not reside in uniform buffers.
    uint32_t ComputeSize () const;
};

MI_NAMESPACE_END

#endif //RHI_PARAM_H
