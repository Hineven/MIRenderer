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
    kStorageBufferArray,
    kUniformBuffer,
    kUAVTexture,
    kSRVTexture,
    kUAVTextureArray,
    kSRVTextureArray,
    kSampler,
    kAccelerationStructure,
    kVertexAttribute,
    // The following types have no mapping in hlsl, just corporate with RDG shader reflection.
    kRenderTarget,
    kVertexBuffer,
    kIndexBuffer,
    kDispatchCommand, // TODO remove this
    kMax,
};

FORCEINLINE std::string ToString (RHIParamType type) {
    switch (type) {
        case RHIParamType::kStorageBuffer: return "StorageBuffer";
        case RHIParamType::kStorageBufferArray: return "StorageBufferArray";
        case RHIParamType::kUniformBuffer: return "UniformBuffer";
        case RHIParamType::kUAVTexture: return "UAVTexture";
        case RHIParamType::kUAVTextureArray: return "UAVTextureArray";
        case RHIParamType::kSRVTexture: return "SRVTexture";
        case RHIParamType::kSRVTextureArray: return "SRVTextureArray";
        case RHIParamType::kSampler: return "Sampler";
        case RHIParamType::kAccelerationStructure: return "AccelerationStructure";
        case RHIParamType::kRenderTarget: return "RenderTarget";
        case RHIParamType::kVertexAttribute: return "VertexAttribute";
        case RHIParamType::kVertexBuffer: return "VertexBuffer";
        case RHIParamType::kIndexBuffer: return "IndexBuffer";
        case RHIParamType::kDispatchCommand: return "DispatchCommand";
        default: return "Unknown";
    }
}


FORCEINLINE RHIParamType RHITypeNameStringToParamType (std::string_view type) {
    if(type == "Texture2D") return RHIParamType::kSRVTexture;
    if(type == "TextureCube" || type == "Texture2DArray") return RHIParamType::kSRVTextureArray;
    if(type == "RWTexture2DArray") return RHIParamType::kUAVTextureArray;
    if(type == "RWTexture2D") return RHIParamType::kUAVTexture;
    if(type == "SamplerState") return RHIParamType::kSampler;
    if(type == "Buffer") return RHIParamType::kStorageBuffer;
    if(type == "Buffer[]") return RHIParamType::kStorageBufferArray;
    if(type == "StructuredBuffer") return RHIParamType::kStorageBuffer;
    if(type == "StructuredBuffer[]") return RHIParamType::kStorageBufferArray;
    if(type == "RWBuffer") return RHIParamType::kStorageBuffer;
    if(type == "RWBuffer[]") return RHIParamType::kStorageBufferArray;
    if(type == "RWStructuredBuffer") return RHIParamType::kStorageBuffer;
    if(type == "RWStructuredBuffer[]") return RHIParamType::kStorageBufferArray;
    if(type == "ConstantBuffer") return RHIParamType::kUniformBuffer;
    if(type == "AccelerationStructure") return RHIParamType::kAccelerationStructure;
    // The following 4 types have no mapping in hlsl, just corporate with RDG shader reflection.
    if(type == "RenderTarget") return RHIParamType::kRenderTarget;
    if(type == "VertexAttribute") return RHIParamType::kVertexAttribute;
    if(type == "VertexBuffer") return RHIParamType::kVertexBuffer;
    if(type == "IndexBuffer") return RHIParamType::kIndexBuffer;
    if(type == "DispatchCommand") return RHIParamType::kDispatchCommand;
    assert(false);
    return RHIParamType::kMax;
}

FORCEINLINE RHIGPUAccessFlags TypeNameStringToRHIAccessFlags (std::string_view type) {
    bool write = false;
    if (type.length() >= 2 && type.starts_with("RW")) {
        write = true;
    }
    auto view = type.substr(write ? 2 : 0);
    if (view.starts_with("Buffer")
        || view.starts_with("StructuredBuffer")) {
        return write ? RHIGPUAccessFlagBits::kShaderStorageRW : RHIGPUAccessFlagBits::kShaderStorageRead;
    }
    if (view.starts_with("Texture2D")
        || view.starts_with("TextureCube")
        || view.starts_with("Texture2DArray")) {
        // texture.SampleXXX() is translated into OpImageSampleXXX, which is considered a sampled read operation.
        // texture.Load is translated into OpImageRead, which is considered a storage read operation.
        return write ? RHIGPUAccessFlagBits::kShaderStorageRW : (RHIGPUAccessFlagBits::kShaderStorageRead | RHIGPUAccessFlagBits::kShaderSampledRead);
    }
    if (view == "SamplerState") {
        return RHIGPUAccessFlagBits::kNone;
    }
    if (view == "AccelerationStructure") {
        return RHIGPUAccessFlagBits::kAccelerationStructureRead;
    }
    if (view == "ConstantBuffer") {
        return RHIGPUAccessFlagBits::kUniformRead;
    }
    assert(false && "Unknown type for RHIAccessFlags conversion");
    return RHIGPUAccessFlagBits::kNone;
}

struct RHIParamStructInfo ;

struct RHIParamInfo {
    std::string name;
    RHIParamType type;
    // Only valid for storage buffers. Otherwise, it can be any value.
    RHIGPUAccessFlags access_flags;
    // Only valid for uniform buffers. Size of the ub struct.
    uint32_t size;
};

struct RHIParamStructInfo {
    byte_strided_span<RHIParamInfo> members;
};

MI_NAMESPACE_END

#endif //RHI_PARAM_H
