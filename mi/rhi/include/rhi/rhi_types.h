/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_TYPES_H
#define MIRENDERERDEV_RHI_TYPES_H

#include "rhi/rhi_common.h"
MI_NAMESPACE_BEGIN

enum class RHIType {
    kVulkan,
};
enum class RHIBindPointType {
    kGraphics,
    kCompute,
    kRayTracing,
    kMax
};

#define MAKE_FLAGS(FlagName) \
struct FlagName##Flags { \
    uint32_t flags; \
    FORCEINLINE FlagName##Flags() : flags(0) {} \
    FORCEINLINE FlagName##Flags(FlagName##FlagBits flag) : flags(static_cast<uint32_t>(flag)) {}  \
    FORCEINLINE FlagName##Flags(uint32_t flags) : flags(flags) {}  \
    FORCEINLINE operator bool() const { return flags != 0; }                                  \
    FORCEINLINE explicit operator unsigned () const { return flags; }                          \
    FORCEINLINE bool operator==(FlagName##Flags other) const { return flags == other.flags; } \
    FORCEINLINE bool operator!=(FlagName##Flags other) const { return flags != other.flags; } \
}; \
FORCEINLINE FlagName##Flags operator|(FlagName##FlagBits a, FlagName##FlagBits b) { \
    return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##FlagBits a, FlagName##FlagBits b) { \
    return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator|(FlagName##Flags a, FlagName##FlagBits b) { \
    return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##Flags a, FlagName##FlagBits b) { \
    return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator|(FlagName##FlagBits a, FlagName##Flags b) { \
    return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); \
} \
FORCEINLINE FlagName##Flags operator&(FlagName##FlagBits a, FlagName##Flags b) { \
    return static_cast<FlagName##Flags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); \
}

enum class RHIShaderFrequencyFlagBits : uint32_t {
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

enum class RHIBufferUsageFlagBits : uint32_t {
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
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIBufferUsage)

enum class RHIGPUAccessFlagBits : uint32_t {
    kRead = 1<<0,
    kWrite = 1<<1,
    kRW = kRead | kWrite,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIGPUAccess)

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

enum class RHITextureType {
    k2D,
    k2DArray,
    k3D,
    kCube,
    k3DArray,
    kMax
};

enum class RHITextureUsageFlagBits : uint32_t {
    kRenderTarget = 1 << 0,
    kDepth = 1 << 1,
    kShaderResource = 1 << 2,
    kUnorderedAccess = 1 << 3,
    kTransferSrc = 1 << 4,
    kTransferDst = 1 << 5,
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
enum class RHIBindlessResourceType {
    kUniformBuffer = 0,
    kStorageBuffer,
    kUAV,
    kSRV,
    kSampler,
    kImmutableSampler,
    kAccelerationStructure,
    kMax
};

FORCEINLINE RHIPipelineResourceType ToPipelineResourceType (RHIBindlessResourceType usage) {
    switch(usage) {
        case RHIBindlessResourceType::kUniformBuffer:
            return RHIPipelineResourceType::kUniformBuffer;
        case RHIBindlessResourceType::kStorageBuffer:
            return RHIPipelineResourceType::kStorageBuffer;
        case RHIBindlessResourceType::kUAV:
            return RHIPipelineResourceType::kUAV;
        case RHIBindlessResourceType::kSRV:
            return RHIPipelineResourceType::kSRV;
        case RHIBindlessResourceType::kSampler:
            return RHIPipelineResourceType::kSampler;
        case RHIBindlessResourceType::kImmutableSampler:
            return RHIPipelineResourceType::kImmutableSampler;
        case RHIBindlessResourceType::kAccelerationStructure:
            return RHIPipelineResourceType::kAccelerationStructure;
        default:
            return RHIPipelineResourceType::kMax;
    }
}

FORCEINLINE RHIBindlessResourceType ToBindlessResourceType (RHIPipelineResourceType usage) {
    switch(usage) {
        case RHIPipelineResourceType::kUniformBuffer:
            return RHIBindlessResourceType::kUniformBuffer;
        case RHIPipelineResourceType::kStorageBuffer:
            return RHIBindlessResourceType::kStorageBuffer;
        case RHIPipelineResourceType::kUAV:
            return RHIBindlessResourceType::kUAV;
        case RHIPipelineResourceType::kSRV:
            return RHIBindlessResourceType::kSRV;
        case RHIPipelineResourceType::kSampler:
            return RHIBindlessResourceType::kSampler;
        case RHIPipelineResourceType::kImmutableSampler:
            return RHIBindlessResourceType::kImmutableSampler;
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

enum class RHIFlushFrameBlockingType {
    kNonBlocking,
    // Return after all command queues completed translation
    kWaitForTranslation,
    // Return after all command queues completed execution (but resources may not be freed)
    kWaitForExecution
};

enum class RHIResourceFlagBits {
    // This resource is imported from an external handle not manager by RHI.
    // Imported resource won't be actually released on the device by RHI if
    // their reference counter drops to 0. And sometimes they have harder
    // usage restrictions.
    kImported
};

MAKE_FLAGS(RHIResource)

#undef MAKE_FLAGS

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_TYPES_H
