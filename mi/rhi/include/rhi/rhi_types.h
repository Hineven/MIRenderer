/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_TYPES_H
#define MIRENDERERDEV_RHI_TYPES_H

#include <format>
#include "core/pixel_format.h"
#include "core/types.h"
#include "rhi/rhi_common.h"

MI_NAMESPACE_BEGIN

enum class RHIType {
    kVulkan,
};
enum class RHIBindPointType : uint32_t {
    kGraphics = 0,
    kCompute,
    kRayTracing,
    kMax
};

enum class RHIShaderFrequencyFlagBits : uint32_t {
    kNone = 0,
    kVertex = 1u<<0,
    kFragment = 1u<<1,
    kGeometry = 1u<<2,
    kTask = 1u<<3,
    kMesh = 1u<<4,
    kCompute = 1u<<5,
    kRayGen = 1u<<6,
    kMiss = 1u<<7,
    kClosestHit = 1u<<8,
    kAnyHit = 1u<<9,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIShaderFrequency)

enum class RHIPipelineStageFlagBits : uint32_t {
    // No stages to wait / barrier
    kNone = 0,
    // Geom, vert, tess, depth, frag, fbo write...
    kOrdinaryGraphics = 1u<<0,
    // Compute
    kCompute = 1u<<1,
    // Ray tracing
    kRayTracing = 1u<<2,
    // Task, mesh
    kTaskMesh = 1u<<3,
    // transfer, copies
    kTransfer = 1u<<4,
    // indirect dispatch...
    kIndirect = 1u<<5,
    // update/build accel
    kAccelBuild = 1u<<6,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIPipelineStage);

enum class RHIBufferUsageFlagBits : uint32_t {
    kNone = 0,
    // Used in draw calls
    kVertex = 1u<<0,
    // Used in draw calls
    kIndex = 1u<<1,
    // Used for constant uniform buffers
    kUniform = 1u<<2,
    // Used for storage buffers
    kStorage = 1u<<3,
    // Used for indirect dispatch commands
    kIndirect = 1u<<4,
    // Used for reading back data from the GPU
    kReadback = 1u<<5,
    // Used for uploading data to the GPU
    kStaging = 1u<<6,
    // This buffer can be a source of copy command
    // (Default enabled with kStorage, and disabled for the rest)
    kTransferSrc = 1u<<7,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIBufferUsage)

struct RHIBufferDesc {
    size_t size;
    RHIBufferUsageFlags usage;
};

enum class RHIGPUAccessFlagBits : uint32_t {
    kNone = 0,
    kRead = 1<<0,
    kWrite = 1<<1,
    kRW = kRead | kWrite,
    kAll = kRW
};
MAKE_FLAGS(RHIGPUAccess)

FORCEINLINE std::string ToString (RHIGPUAccessFlags flags) {
    std::string result = "";
    if (flags & RHIGPUAccessFlagBits::kRead) {
        result += "Read";
    }
    if (flags & RHIGPUAccessFlagBits::kWrite) {
        if (!result.empty()) result += " | ";
        result += "Write";
    }
    if (result.empty()) result = "None";
    return result;
}

enum class RHISamplerAddressModeType {
    kRepeat,
    kClampToEdge,
    kClampToBorder,
    kMax
};

enum class RHISamplerFilterType {
    kNearest,
    kLinear,
    kPoint,
    kMax
};

enum class RHIPrimitiveType {
    kTriangle,
    kLine,
    kPoint,
    kMax
};

enum class RHIPipelineType {
    kGraphics,
    kCompute,
    kRayTracing,
    kMax
};

enum class RHITextureType {
    k2D,
    k2DArray,
    k3D,
    kCube,
    k3DArray,
    kMax
};

enum class RHITextureUsageFlagBits : uint32_t {
    kNone = 0,
    kRenderTarget = 1 << 0,
    kDepthStencil = 1 << 1,
    kShaderResource = 1 << 2,
    kUnorderedAccess = 1 << 3,
    kTransferSrc = 1 << 4,
    kTransferDst = 1 << 5,
    kTransfer = kTransferSrc | kTransferDst,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHITextureUsage)

enum class RHIPipelineResourceType {
    kUniformBuffer,
    kStorageBuffer,
    kUAV,
    kSRV,
    kSampler,
    kImmutableSampler,
    kAccelerationStructure,
    kMax
};

// Resource types compatible with bindless design
enum class RHIBindlessResourceType : unsigned {
    kReadOnlyStorageBuffer = 0,
    kSRV,
    kAccelerationStructure,
    kMax
};

FORCEINLINE RHIPipelineResourceType ToPipelineResourceType (RHIBindlessResourceType usage) {
    switch(usage) {
        case RHIBindlessResourceType::kReadOnlyStorageBuffer:
            return RHIPipelineResourceType::kStorageBuffer;
        case RHIBindlessResourceType::kSRV:
            return RHIPipelineResourceType::kSRV;
        case RHIBindlessResourceType::kAccelerationStructure:
            return RHIPipelineResourceType::kAccelerationStructure;
        default:
            return RHIPipelineResourceType::kMax;
    }
}

FORCEINLINE RHIBindlessResourceType ToBindlessResourceType (RHIPipelineResourceType usage) {
    switch(usage) {
        case RHIPipelineResourceType::kStorageBuffer:
            return RHIBindlessResourceType::kReadOnlyStorageBuffer;
        case RHIPipelineResourceType::kSRV:
            return RHIBindlessResourceType::kSRV;
        case RHIPipelineResourceType::kAccelerationStructure:
            return RHIBindlessResourceType::kAccelerationStructure;
        default:
            return RHIBindlessResourceType::kMax;
    }
}

enum class RHIShaderIRType {
    kSPIRV,
    // no other types for now
    kMax
};

enum class RHIVertexInputRateType {
    kVertex,
    kInstance,
    kMax
};

enum class RHIVertexAttributeFormatType {
    k1xFp32,
    k2xFp32,
    k3xFp32,
    k4xFp32,
    kMax
};

FORCEINLINE std::string ToString (RHIVertexAttributeFormatType type) {
    switch (type) {
        case RHIVertexAttributeFormatType::k1xFp32:
            return "1xfp32";
        case RHIVertexAttributeFormatType::k2xFp32:
            return "2xfp32";
        case RHIVertexAttributeFormatType::k3xFp32:
            return "3xfp32";
        case RHIVertexAttributeFormatType::k4xFp32:
            return "4xfp32";
        default:
            return "unknown";
    }
}

FORCEINLINE uint32_t GetVertexAttributeFormatSize (RHIVertexAttributeFormatType type) {
    switch(type) {
        case RHIVertexAttributeFormatType::k1xFp32:
            return 4;
        case RHIVertexAttributeFormatType::k2xFp32:
            return 8;
        case RHIVertexAttributeFormatType::k3xFp32:
            return 12;
        case RHIVertexAttributeFormatType::k4xFp32:
            return 16;
        default:
            return 0;
    }
}

enum class RHIFragmentOutputFormatType {
    k4xFp32,
    // Signed / unsigned all considered as 32bit integer
    k4xUIint32,
    kMax
};

FORCEINLINE std::string ToString (RHIFragmentOutputFormatType type) {
    switch (type) {
        case RHIFragmentOutputFormatType::k4xFp32:
            return "4xfp32";
        case RHIFragmentOutputFormatType::k4xUIint32:
            return "4xuint32";
        default:
            return "unknown";
    }
}

FORCEINLINE bool RHIIsOutputCompatiablePixelFormat (RHIFragmentOutputFormatType output, PixelFormatType type) {
    if (IsFloatPixelFormat(type) && output == RHIFragmentOutputFormatType::k4xFp32) {
        return true;
    }
    if (IsUIntPixelFormat(type) && output == RHIFragmentOutputFormatType::k4xUIint32) {
        return true;
    }
    return false;
}

FORCEINLINE const char * GetRHIFragmentOutputFormatName (RHIFragmentOutputFormatType type) {
    switch(type) {
        case RHIFragmentOutputFormatType::k4xFp32:
            return "4xfp32";
        case RHIFragmentOutputFormatType::k4xUIint32:
            return "4xinteger32";
        default:
            return "unknown";
    }
}

enum class RHIIndexType {
    kUint16,
    kUint32,
    kMax
};

enum class RHIPrimitiveTopologyType {
    kTriangleList,
    kTriangleStrip,
    kLineList,
    kLineStrip,
    kPointList,
    kMax
};

enum class RHIDepthCompareOpType {
    kNever,
    kLess,
    kEqual,
    kLessOrEqual,
    kGreater,
    kNotEqual,
    kGreaterOrEqual,
    kAlways,
    kMax
};

enum class RHICullModeType {
    kNone,
    kFront,
    kBack,
    kMax
};

enum class RHIFrontFaceType {
    kClockwise,
    kCounterClockwise,
    kMax
};

enum class RHIBlendFactorType {
    kZero,
    kOne,
    kSrcColor,
    kOneMinusSrcColor,
    kDstColor,
    kOneMinusDstColor,
    kSrcAlpha,
    kOneMinusSrcAlpha,
    kDstAlpha,
    kOneMinusDstAlpha,
    kConstantColor,
    kOneMinusConstantColor,
    kConstantAlpha,
    kOneMinusConstantAlpha,
    kSrcAlphaSaturate,
    kSrc1Color,
    kOneMinusSrc1Color,
    kSrc1Alpha,
    kOneMinusSrc1Alpha,
    kMax
};

enum class RHIBlendOpType {
    kBlendAdd,
    kBlendSubtract,
    kBlendReverseSubtract,
    kBlendMin,
    kBlendMax,
    kMax
};

enum class RHILoadOpType {
    kLoad,
    kClear,
    kDontCare,
    kMax
};

enum class RHIStoreOpType {
    kStore,
    kDontCare,
    kMax
};

enum class RHICommandQueueType {
    kGraphics,
    kMax
};

enum class RHIResourceFlagBits {
    kNone = 0,
    // This resource is imported from an external handle not manager by RHI.
    // Imported resource won't be actually released on the device by RHI if
    // their reference counter drops to 0. And sometimes they have harder
    // usage restrictions.
    kImported = 1 << 0
};

MAKE_FLAGS(RHIResource)

enum class RHITextureLayoutType {
    kUndefined,
    kShaderReadOnlyOptimal,
    kColorAttachment,
    kDepthStencilAttachment,
    kTransferSrcOptimal,
    kTransferDstOptimal,
    kGeneral,
    kMax
};


MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_TYPES_H
