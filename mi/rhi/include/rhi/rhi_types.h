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
    kRaygen = 1u<<6,
    kMiss = 1u<<7,
    kClosestHit = 1u<<8,
    kAnyHit = 1u<<9,
    kIntersection = 1u<<10,
    kCallable = 1u<<11,
    kRayTracing = kRaygen | kMiss | kClosestHit | kAnyHit | kIntersection | kCallable,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIShaderFrequency)

enum class RHIPipelineStageFlagBits : uint32_t {
    // No stages to wait / barrier
    kNone = 0,
    kVertex = 1u<<0,        // Vertex stage (vertex/index input, vertex shader)
    kGeometry = 1u<<1,      // Geometry stage
    kTess = 1u<<2,         // Tesselation stage (control / eval)
    kFragment = 1u<<3,      // Fragment stage (framebuffer output not included!)
    kFramebufferOutput = 1u<<4, // Framebuffer output stage
    // Geom, vert, tess, depth, frag, fbo write...
    kOrdinaryGraphics =
        kVertex | kFragment | kFramebufferOutput | kGeometry | kTess,
    // Task & mesh
    kTaskMesh = 1u<<5,
    // All graphics including tesselation, task and mesh shaders
    kAllGraphics = kOrdinaryGraphics | kTess | kTaskMesh,
    // Compute
    kCompute = 1u<<6,
    // Acceleration structure build
    kAccelerationStructureBuild = 1u<<7,
    // Ray tracing shaders
    kRayTracing = 1u<<8,
    // transfer, copies, blits...
    kTransfer = 1u<<9,
    // indirect dispatch...
    kIndirect = 1u<<10,
    // Only commonly seen stages are abstracted. Some rare stages are not included above
    // If you don't know what to use, use this. This is mapped to all stages.
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
    // Ray tracing related buffer usages
    kAccelerationStructureStorage = 1u<<8,
    kAccelerationStructureBuildInput = 1u<<9,
    kShaderBindingTable = 1u<<10,
    kAccelerationStructureScratch = 1u<<11,
    // The buffer can have an address in shaders
    kShaderDeviceAddress = 1u<<12,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHIBufferUsage)

struct RHIBufferDesc {
    size_t size;
    RHIBufferUsageFlags usage;
};

enum class RHIGPUAccessFlagBits : uint32_t {
    kNone = 0,
    kIndirectCommandRead = 1u<<0,
    kIndexRead = 1u<<1,
    kVertexAttributeRead = 1u<<2,
    kUniformRead = 1u<<3,
    // UAV (storage texture / buffer)
    kShaderStorageRead = 1u<<4,
    // SRV (sampled texture)
    kShaderSampledRead = 1u<<5,
    // Read acceleration structures in shader code
    kAccelerationStructureRead = 1u<<6,
    // All possible reads in shader code EXCEPT uniform buffer read, as well as build input read for acceleration structures
    kShaderRead = kShaderStorageRead | kShaderSampledRead | kAccelerationStructureRead,
    kTransferRead = 1u<<7,
    kDepthStencilRead = 1u<<8,
    kColorAttachmentRead = 1u<<9,
    kShaderBindingTableRead = 1u<<10,
    // Only commonly seen read accesses are abstraced. Some rare read accesses are not included above
    // (such as eInputAttachmentRead. Subpass inputs are not commonly seen in desktop environments)
    // If you don't know what to use, use this. This is mapped to all read operations.
    kRead = 0xFFFFu,
    kShaderWrite = 1u<<16,
    kAccelerationStructureWrite = 1u<<17,
    kDepthStencilWrite = 1u<<18,
    kColorAttachmentWrite = 1u<<19,
    kTransferWrite = 1u<<20,
    kWrite = 0xFFFF0000u,
    kShaderStorageRW = kShaderStorageRead | kShaderWrite,
    kShaderRW = kShaderRead | kShaderWrite,
    kColorAttachmentRW = kColorAttachmentRead | kColorAttachmentWrite,
    kDepthStencilRW = kDepthStencilRead | kDepthStencilWrite,
    kTransferRW = kTransferRead | kTransferWrite,
    kAccelerationStructureRW = kAccelerationStructureRead | kAccelerationStructureWrite,
    kRW = kRead | kWrite,
    kAll = kRW
};
MAKE_FLAGS(RHIGPUAccess)

FORCEINLINE std::string ToString(RHIGPUAccessFlags flags) {
    if (flags == RHIGPUAccessFlagBits::kNone) return "None";
    std::string result;
    struct FlagName {
        RHIGPUAccessFlagBits bit;
        const char* name;
    };
    static const FlagName flagNames[] = {
        {RHIGPUAccessFlagBits::kIndirectCommandRead, "IndirectCommandRead"},
        {RHIGPUAccessFlagBits::kIndexRead, "IndexRead"},
        {RHIGPUAccessFlagBits::kVertexAttributeRead, "VertexAttributeRead"},
        {RHIGPUAccessFlagBits::kUniformRead, "UniformRead"},
        {RHIGPUAccessFlagBits::kShaderStorageRead, "ShaderStorageRead"},
        {RHIGPUAccessFlagBits::kAccelerationStructureRead, "AccelerationStructureRead"},
        {RHIGPUAccessFlagBits::kShaderRead, "ShaderRead"},
        {RHIGPUAccessFlagBits::kTransferRead, "TransferRead"},
        {RHIGPUAccessFlagBits::kDepthStencilRead, "DepthStencilRead"},
        {RHIGPUAccessFlagBits::kColorAttachmentRead, "ColorAttachmentRead"},
        {RHIGPUAccessFlagBits::kRead, "Read"},
        {RHIGPUAccessFlagBits::kShaderWrite, "ShaderWrite"},
        {RHIGPUAccessFlagBits::kAccelerationStructureWrite, "AccelerationStructureWrite"},
        {RHIGPUAccessFlagBits::kDepthStencilWrite, "DepthStencilWrite"},
        {RHIGPUAccessFlagBits::kColorAttachmentWrite, "ColorAttachmentWrite"},
        {RHIGPUAccessFlagBits::kTransferWrite, "TransferWrite"},
        {RHIGPUAccessFlagBits::kWrite, "Write"},
        {RHIGPUAccessFlagBits::kShaderStorageRW, "ShaderStorageRW"},
        {RHIGPUAccessFlagBits::kShaderRW, "ShaderRW"},
        {RHIGPUAccessFlagBits::kColorAttachmentRW, "ColorAttachmentRW"},
        {RHIGPUAccessFlagBits::kDepthStencilRW, "DepthStencilRW"},
        {RHIGPUAccessFlagBits::kTransferRW, "TransferRW"},
        {RHIGPUAccessFlagBits::kRW, "RW"},
        {RHIGPUAccessFlagBits::kAll, "All"},
    };
    bool first = true;
    for (const auto& fn : flagNames) {
        if ((flags & fn.bit) == fn.bit) {
            if (!first) result += " | ";
            result += fn.name;
            first = false;
        }
    }
    return result.empty() ? "None" : result;
}

enum class RHISamplerAddressModeType {
    kRepeat = 0,
    kClampToEdge,
    kClampToBorder,
    kMax
};

enum class RHISamplerFilterType {
    kLinear = 0,
    kPoint,
    kMax
};

enum class RHIPrimitiveType {
    kTriangle = 0,
    kLine,
    kPoint,
    kMax
};

enum class RHIPipelineType {
    kGraphics = 0,
    kCompute,
    kRayTracing,
    kMax
};

enum class RHITextureType {
    k2D = 0,
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
    // Sampled texture
    kShaderResource = 1 << 2,
    // Storage texture
    kUnorderedAccess = 1 << 3,
    kTransferSrc = 1 << 4,
    kTransferDst = 1 << 5,
    kTransfer = kTransferSrc | kTransferDst,
    kAll = 0xffffffffu
};
MAKE_FLAGS(RHITextureUsage)

enum class RHIPipelineResourceType {
    kUniformBuffer = 0,
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
    kVolumeSRV,
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
    kSPIRV = 0,
    // no other types for now
    kMax
};

enum class RHIVertexInputRateType {
    kVertex = 0,
    kInstance,
    kMax
};

enum class RHIVertexAttributeFormatType {
    k1xFp32 = 0,
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
    kUint32 = 0,
    kUint16,
    kMax
};

enum class RHIPrimitiveTopologyType {
    kTriangleList = 0,
    kTriangleStrip,
    kLineList,
    kLineStrip,
    kPointList,
    kMax
};

enum class RHIPolygonModeType {
    kFill = 0,
    kLine,
    kPoint,
    kMax
};

enum class RHIDepthCompareOpType {
    kNever = 0,
    kLess,
    kEqual,
    kLessOrEqual,
    kGreater,
    kNotEqual,
    kGreaterOrEqual,
    kAlways,
    kMax
};

enum class RHICullModeType : uint32_t {
    kNone = 0, // No culling
    kFront, // culling front
    kBack, //  culling back
    kMax
};

enum class RHIFrontFaceType {
    kCounterClockwise = 0,
    kClockwise,
    kMax
};

enum class RHIBlendFactorType {
    kZero = 0,
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
    kBlendAdd = 0,
    kBlendSubtract,
    kBlendReverseSubtract,
    kBlendMin,
    kBlendMax,
    kMax
};

enum class RHILoadOpType {
    kLoad = 0,
    kClear,
    kDontCare,
    kMax
};

enum class RHIStoreOpType {
    kStore = 0,
    kDontCare,
    kMax
};

enum class RHICommandQueueType {
    kGraphics = 0,
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
    kUndefined = 0,
    kShaderReadOnlyOptimal,
    kColorAttachment,
    kDepthStencilAttachment,
    kTransferSrcOptimal,
    kTransferDstOptimal,
    kGeneral,
    kMax
};

// Ray tracing shader group types
enum class RHIRayTracingShaderGroupType {
    kRayGeneration = 0, // Ray generation shader group
    kMiss,              // Miss shader group
    kTrianglesHitGroup, // Hit group for triangle geometry
    kProceduralHitGroup,// Hit group for procedural geometry
    kCallable,          // Callable shader group
    kMax
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_TYPES_H
