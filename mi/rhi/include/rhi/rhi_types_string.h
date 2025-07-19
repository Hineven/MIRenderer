/*
 * Created: 2024/7/29
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_TYPES_STRING_H
#define MIRENDERERDEV_RHI_TYPES_STRING_H

#include "rhi/rhi_types.h"
#include <string>

MI_NAMESPACE_BEGIN

// RHIType
FORCEINLINE std::string ToString(RHIType type) {
    switch (type) {
        case RHIType::kVulkan:
            return "Vulkan";
        default:
            return "Unknown";
    }
}

// RHIBindPointType
FORCEINLINE std::string ToString(RHIBindPointType type) {
    switch (type) {
        case RHIBindPointType::kGraphics:
            return "Graphics";
        case RHIBindPointType::kCompute:
            return "Compute";
        case RHIBindPointType::kRayTracing:
            return "RayTracing";
        default:
            return "Unknown";
    }
}

// RHIShaderFrequencyFlagBits
FORCEINLINE std::string ToString(RHIShaderFrequencyFlagBits bit) {
    switch (bit) {
        case RHIShaderFrequencyFlagBits::kVertex:
            return "Vertex";
        case RHIShaderFrequencyFlagBits::kFragment:
            return "Fragment";
        case RHIShaderFrequencyFlagBits::kGeometry:
            return "Geometry";
        case RHIShaderFrequencyFlagBits::kTask:
            return "Task";
        case RHIShaderFrequencyFlagBits::kMesh:
            return "Mesh";
        case RHIShaderFrequencyFlagBits::kCompute:
            return "Compute";
        case RHIShaderFrequencyFlagBits::kRaygen:
            return "RayGen";
        case RHIShaderFrequencyFlagBits::kMiss:
            return "Miss";
        case RHIShaderFrequencyFlagBits::kClosestHit:
            return "ClosestHit";
        case RHIShaderFrequencyFlagBits::kAnyHit:
            return "AnyHit";
        case RHIShaderFrequencyFlagBits::kAll:
            return "All";
        default:
            return "Unknown";
    }
}

// RHIShaderFrequencyFlags
FORCEINLINE std::string ToString(RHIShaderFrequencyFlags flags) {
    if (flags == RHIShaderFrequencyFlagBits::kNone) {
        return "None";
    }
    if (flags == RHIShaderFrequencyFlagBits::kAll) {
        return "All";
    }
    
    std::string result;
    bool first = true;
    
    for (uint32_t i = 0; i < 32; ++i) {
        RHIShaderFrequencyFlagBits bit = static_cast<RHIShaderFrequencyFlagBits>(1u << i);
        if (flags & bit) {
            if (!first) {
                result += " | ";
            }
            result += ToString(bit);
            first = false;
        }
    }

    return result;
}

// RHIPipelineStageFlagBits
FORCEINLINE std::string ToString(RHIPipelineStageFlagBits bit) {
    switch (bit) {
        case RHIPipelineStageFlagBits::kNone:
            return "None";
        case RHIPipelineStageFlagBits::kOrdinaryGraphics:
            return "OrdinaryGraphics";
        case RHIPipelineStageFlagBits::kCompute:
            return "Compute";
        case RHIPipelineStageFlagBits::kRayTracing:
            return "RayTracing";
        case RHIPipelineStageFlagBits::kTaskMesh:
            return "TaskMesh";
        case RHIPipelineStageFlagBits::kTransfer:
            return "Transfer";
        case RHIPipelineStageFlagBits::kIndirect:
            return "Indirect";
        case RHIPipelineStageFlagBits::kAccelerationStructureBuild:
            return "AccelerationStructureBuild";
        case RHIPipelineStageFlagBits::kAll:
            return "All";
        default:
            return "Unknown";
    }
}

// RHIPipelineStageFlags
FORCEINLINE std::string ToString(RHIPipelineStageFlags flags) {
    if (flags == RHIPipelineStageFlagBits::kNone) {
        return "None";
    }
    if (flags == RHIPipelineStageFlagBits::kAll) {
        return "All";
    }
    
    std::string result;
    bool first = true;
    
    for (uint32_t i = 0; i < 32; ++i) {
        RHIPipelineStageFlagBits bit = static_cast<RHIPipelineStageFlagBits>(1u << i);
        if (flags & bit) {
            if (!first) {
                result += " | ";
            }
            result += ToString(bit);
            first = false;
        }
    }
    
    return result;
}

// RHIBufferUsageFlagBits
FORCEINLINE std::string ToString(RHIBufferUsageFlagBits bit) {
    switch (bit) {
        case RHIBufferUsageFlagBits::kVertex:
            return "Vertex";
        case RHIBufferUsageFlagBits::kIndex:
            return "Index";
        case RHIBufferUsageFlagBits::kUniform:
            return "Uniform";
        case RHIBufferUsageFlagBits::kStorage:
            return "Storage";
        case RHIBufferUsageFlagBits::kIndirect:
            return "Indirect";
        case RHIBufferUsageFlagBits::kReadback:
            return "Readback";
        case RHIBufferUsageFlagBits::kStaging:
            return "Staging";
        case RHIBufferUsageFlagBits::kTransferSrc:
            return "TransferSrc";
        case RHIBufferUsageFlagBits::kAll:
            return "All";
        default:
            return "Unknown";
    }
}

// RHIBufferUsageFlags
FORCEINLINE std::string ToString(RHIBufferUsageFlags flags) {
    if (flags == RHIBufferUsageFlagBits::kNone) {
        return "None";
    }
    if (flags == RHIBufferUsageFlagBits::kAll) {
        return "All";
    }
    
    std::string result;
    bool first = true;
    
    for (uint32_t i = 0; i < 32; ++i) {
        RHIBufferUsageFlagBits bit = static_cast<RHIBufferUsageFlagBits>(1u << i);
        if (flags & bit) {
            if (!first) {
                result += " | ";
            }
            result += ToString(bit);
            first = false;
        }
    }
    
    return result;
}

// RHISamplerAddressModeType
FORCEINLINE std::string ToString(RHISamplerAddressModeType type) {
    switch (type) {
        case RHISamplerAddressModeType::kRepeat:
            return "Repeat";
        case RHISamplerAddressModeType::kClampToEdge:
            return "ClampToEdge";
        case RHISamplerAddressModeType::kClampToBorder:
            return "ClampToBorder";
        default:
            return "Unknown";
    }
}

// RHISamplerFilterType
FORCEINLINE std::string ToString(RHISamplerFilterType type) {
    switch (type) {
        case RHISamplerFilterType::kNearest:
            return "Nearest";
        case RHISamplerFilterType::kLinear:
            return "Linear";
        case RHISamplerFilterType::kPoint:
            return "Point";
        default:
            return "Unknown";
    }
}

// RHIPrimitiveType
FORCEINLINE std::string ToString(RHIPrimitiveType type) {
    switch (type) {
        case RHIPrimitiveType::kTriangle:
            return "Triangle";
        case RHIPrimitiveType::kLine:
            return "Line";
        case RHIPrimitiveType::kPoint:
            return "Point";
        default:
            return "Unknown";
    }
}

// RHIPipelineType
FORCEINLINE std::string ToString(RHIPipelineType type) {
    switch (type) {
        case RHIPipelineType::kGraphics:
            return "Graphics";
        case RHIPipelineType::kCompute:
            return "Compute";
        case RHIPipelineType::kRayTracing:
            return "RayTracing";
        default:
            return "Unknown";
    }
}

// RHITextureType
FORCEINLINE std::string ToString(RHITextureType type) {
    switch (type) {
        case RHITextureType::k2D:
            return "2D";
        case RHITextureType::k2DArray:
            return "2DArray";
        case RHITextureType::k3D:
            return "3D";
        case RHITextureType::kCube:
            return "Cube";
        case RHITextureType::k3DArray:
            return "3DArray";
        default:
            return "Unknown";
    }
}

// RHITextureUsageFlagBits
FORCEINLINE std::string ToString(RHITextureUsageFlagBits bit) {
    switch (bit) {
        case RHITextureUsageFlagBits::kRenderTarget:
            return "RenderTarget";
        case RHITextureUsageFlagBits::kDepthStencil:
            return "DepthStencil";
        case RHITextureUsageFlagBits::kShaderResource:
            return "ShaderResource";
        case RHITextureUsageFlagBits::kUnorderedAccess:
            return "UnorderedAccess";
        case RHITextureUsageFlagBits::kTransferSrc:
            return "TransferSrc";
        case RHITextureUsageFlagBits::kTransferDst:
            return "TransferDst";
        case RHITextureUsageFlagBits::kTransfer:
            return "Transfer";
        case RHITextureUsageFlagBits::kAll:
            return "All";
        default:
            return "Unknown";
    }
}

// RHITextureUsageFlags
FORCEINLINE std::string ToString(RHITextureUsageFlags flags) {
    if (flags == RHITextureUsageFlagBits::kNone) {
        return "None";
    }
    if (flags == RHITextureUsageFlagBits::kAll) {
        return "All";
    }
    
    std::string result;
    bool first = true;
    
    for (uint32_t i = 0; i < 32; ++i) {
        RHITextureUsageFlagBits bit = static_cast<RHITextureUsageFlagBits>(1u << i);
        if (flags & bit) {
            if (!first) {
                result += " | ";
            }
            result += ToString(bit);
            first = false;
        }
    }
    
    return result;
}

// RHIPipelineResourceType
FORCEINLINE std::string ToString(RHIPipelineResourceType type) {
    switch (type) {
        case RHIPipelineResourceType::kUniformBuffer:
            return "UniformBuffer";
        case RHIPipelineResourceType::kStorageBuffer:
            return "StorageBuffer";
        case RHIPipelineResourceType::kUAV:
            return "UAV";
        case RHIPipelineResourceType::kSRV:
            return "SRV";
        case RHIPipelineResourceType::kSampler:
            return "Sampler";
        case RHIPipelineResourceType::kImmutableSampler:
            return "ImmutableSampler";
        case RHIPipelineResourceType::kAccelerationStructure:
            return "AccelerationStructure";
        default:
            return "Unknown";
    }
}

// RHIBindlessResourceType
FORCEINLINE std::string ToString(RHIBindlessResourceType type) {
    switch (type) {
        case RHIBindlessResourceType::kReadOnlyStorageBuffer:
            return "ReadOnlyStorageBuffer";
        case RHIBindlessResourceType::kSRV:
            return "SRV";
        case RHIBindlessResourceType::kAccelerationStructure:
            return "AccelerationStructure";
        default:
            return "Unknown";
    }
}

// RHIShaderIRType
FORCEINLINE std::string ToString(RHIShaderIRType type) {
    switch (type) {
        case RHIShaderIRType::kSPIRV:
            return "SPIRV";
        default:
            return "Unknown";
    }
}

// RHIVertexInputRateType
FORCEINLINE std::string ToString(RHIVertexInputRateType type) {
    switch (type) {
        case RHIVertexInputRateType::kVertex:
            return "Vertex";
        case RHIVertexInputRateType::kInstance:
            return "Instance";
        default:
            return "Unknown";
    }
}

// RHIIndexType
FORCEINLINE std::string ToString(RHIIndexType type) {
    switch (type) {
        case RHIIndexType::kUint16:
            return "Uint16";
        case RHIIndexType::kUint32:
            return "Uint32";
        default:
            return "Unknown";
    }
}

// RHIPrimitiveTopologyType
FORCEINLINE std::string ToString(RHIPrimitiveTopologyType type) {
    switch (type) {
        case RHIPrimitiveTopologyType::kTriangleList:
            return "TriangleList";
        case RHIPrimitiveTopologyType::kTriangleStrip:
            return "TriangleStrip";
        case RHIPrimitiveTopologyType::kLineList:
            return "LineList";
        case RHIPrimitiveTopologyType::kLineStrip:
            return "LineStrip";
        case RHIPrimitiveTopologyType::kPointList:
            return "PointList";
        default:
            return "Unknown";
    }
}

// RHIDepthCompareOpType
FORCEINLINE std::string ToString(RHIDepthCompareOpType type) {
    switch (type) {
        case RHIDepthCompareOpType::kNever:
            return "Never";
        case RHIDepthCompareOpType::kLess:
            return "Less";
        case RHIDepthCompareOpType::kEqual:
            return "Equal";
        case RHIDepthCompareOpType::kLessOrEqual:
            return "LessOrEqual";
        case RHIDepthCompareOpType::kGreater:
            return "Greater";
        case RHIDepthCompareOpType::kNotEqual:
            return "NotEqual";
        case RHIDepthCompareOpType::kGreaterOrEqual:
            return "GreaterOrEqual";
        case RHIDepthCompareOpType::kAlways:
            return "Always";
        default:
            return "Unknown";
    }
}

// RHICullModeType
FORCEINLINE std::string ToString(RHICullModeType type) {
    switch (type) {
        case RHICullModeType::kNone:
            return "None";
        case RHICullModeType::kFront:
            return "Front";
        case RHICullModeType::kBack:
            return "Back";
        default:
            return "Unknown";
    }
}

// RHIFrontFaceType
FORCEINLINE std::string ToString(RHIFrontFaceType type) {
    switch (type) {
        case RHIFrontFaceType::kClockwise:
            return "Clockwise";
        case RHIFrontFaceType::kCounterClockwise:
            return "CounterClockwise";
        default:
            return "Unknown";
    }
}

// RHIBlendFactorType
FORCEINLINE std::string ToString(RHIBlendFactorType type) {
    switch (type) {
        case RHIBlendFactorType::kZero:
            return "Zero";
        case RHIBlendFactorType::kOne:
            return "One";
        case RHIBlendFactorType::kSrcColor:
            return "SrcColor";
        case RHIBlendFactorType::kOneMinusSrcColor:
            return "OneMinusSrcColor";
        case RHIBlendFactorType::kDstColor:
            return "DstColor";
        case RHIBlendFactorType::kOneMinusDstColor:
            return "OneMinusDstColor";
        case RHIBlendFactorType::kSrcAlpha:
            return "SrcAlpha";
        case RHIBlendFactorType::kOneMinusSrcAlpha:
            return "OneMinusSrcAlpha";
        case RHIBlendFactorType::kDstAlpha:
            return "DstAlpha";
        case RHIBlendFactorType::kOneMinusDstAlpha:
            return "OneMinusDstAlpha";
        case RHIBlendFactorType::kConstantColor:
            return "ConstantColor";
        case RHIBlendFactorType::kOneMinusConstantColor:
            return "OneMinusConstantColor";
        case RHIBlendFactorType::kConstantAlpha:
            return "ConstantAlpha";
        case RHIBlendFactorType::kOneMinusConstantAlpha:
            return "OneMinusConstantAlpha";
        case RHIBlendFactorType::kSrcAlphaSaturate:
            return "SrcAlphaSaturate";
        case RHIBlendFactorType::kSrc1Color:
            return "Src1Color";
        case RHIBlendFactorType::kOneMinusSrc1Color:
            return "OneMinusSrc1Color";
        case RHIBlendFactorType::kSrc1Alpha:
            return "Src1Alpha";
        case RHIBlendFactorType::kOneMinusSrc1Alpha:
            return "OneMinusSrc1Alpha";
        default:
            return "Unknown";
    }
}

// RHIBlendOpType
FORCEINLINE std::string ToString(RHIBlendOpType type) {
    switch (type) {
        case RHIBlendOpType::kBlendAdd:
            return "Add";
        case RHIBlendOpType::kBlendSubtract:
            return "Subtract";
        case RHIBlendOpType::kBlendReverseSubtract:
            return "ReverseSubtract";
        case RHIBlendOpType::kBlendMin:
            return "Min";
        case RHIBlendOpType::kBlendMax:
            return "Max";
        default:
            return "Unknown";
    }
}

// RHILoadOpType
FORCEINLINE std::string ToString(RHILoadOpType type) {
    switch (type) {
        case RHILoadOpType::kLoad:
            return "Load";
        case RHILoadOpType::kClear:
            return "Clear";
        case RHILoadOpType::kDontCare:
            return "DontCare";
        default:
            return "Unknown";
    }
}

// RHIStoreOpType
FORCEINLINE std::string ToString(RHIStoreOpType type) {
    switch (type) {
        case RHIStoreOpType::kStore:
            return "Store";
        case RHIStoreOpType::kDontCare:
            return "DontCare";
        default:
            return "Unknown";
    }
}

// RHIResourceFlagBits
FORCEINLINE std::string ToString(RHIResourceFlagBits bit) {
    switch (bit) {
        case RHIResourceFlagBits::kImported:
            return "Imported";
        default:
            return "Unknown";
    }
}

// RHIResourceFlags
FORCEINLINE std::string ToString(RHIResourceFlags flags) {
    if (flags == RHIResourceFlagBits::kNone) {
        return "None";
    }
    
    std::string result;
    bool first = true;
    
    for (uint32_t i = 0; i < 32; ++i) {
        RHIResourceFlagBits bit = static_cast<RHIResourceFlagBits>(1u << i);
        if (flags & bit) {
            if (!first) {
                result += " | ";
            }
            result += ToString(bit);
            first = false;
        }
    }
    
    return result;
}

// RHITextureLayoutType
FORCEINLINE std::string ToString(RHITextureLayoutType type) {
    switch (type) {
        case RHITextureLayoutType::kUndefined:
            return "Undefined";
        case RHITextureLayoutType::kShaderReadOnlyOptimal:
            return "ShaderReadOnlyOptimal";
        case RHITextureLayoutType::kColorAttachment:
            return "ColorAttachment";
        case RHITextureLayoutType::kDepthStencilAttachment:
            return "DepthStencilAttachment";
        case RHITextureLayoutType::kTransferSrcOptimal:
            return "TransferSrcOptimal";
        case RHITextureLayoutType::kTransferDstOptimal:
            return "TransferDstOptimal";
        case RHITextureLayoutType::kGeneral:
            return "General";
        default:
            return "Unknown";
    }
}

FORCEINLINE std::string ToString(RHICommandQueueType type) {
    switch (type) {
        case RHICommandQueueType::kGraphics:
            return "Graphics";
        default:
            return "Unknown";
    }
}

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_TYPES_STRING_H
