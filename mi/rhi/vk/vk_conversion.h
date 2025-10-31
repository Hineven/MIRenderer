/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VK_CONVERSION_H
#define MI_VK_CONVERSION_H

#include "core/pixel_format.h"
#include "core/infra.h"
#include "vk_rhi.h"
MI_NAMESPACE_BEGIN

FORCEINLINE vk::Format GetVulkanPixelFormat (PixelFormatType format) {
    switch(format) {
        case PixelFormatType::kR8_UNORM:
            return vk::Format::eR8Unorm;
        case PixelFormatType::kR8_UINT:
            return vk::Format::eR8Uint;
        case PixelFormatType::kR8G8_UNORM:
            return vk::Format::eR8G8Unorm;
        case PixelFormatType::kB8G8R8A8_UNORM:
            return vk::Format::eB8G8R8A8Unorm;
        case PixelFormatType::kB8G8R8A8_SRGB:
            return vk::Format::eB8G8R8A8Srgb;
        case PixelFormatType::kR8G8B8A8_UNORM:
            return vk::Format::eR8G8B8A8Unorm;
        case PixelFormatType::kR8G8B8A8_SRGB:
            return vk::Format::eR8G8B8A8Srgb;
        case PixelFormatType::kR16G16B16A16_FLOAT:
            return vk::Format::eR16G16B16A16Sfloat;
        case PixelFormatType::kR16G16_FLOAT:
            return vk::Format::eR16G16Sfloat;
        case PixelFormatType::kR32G32B32A32_FLOAT:
            return vk::Format::eR32G32B32A32Sfloat;
        case PixelFormatType::kR32G32B32_FLOAT:
            return vk::Format::eR32G32B32Sfloat;
        case PixelFormatType::kR32G32_FLOAT:
            return vk::Format::eR32G32Sfloat;
        case PixelFormatType::kR32_FLOAT:
            return vk::Format::eR32Sfloat;
        case PixelFormatType::kD32_FLOAT:
            return vk::Format::eD32Sfloat;
        case PixelFormatType::kR32G32B32A32_UINT:
            return vk::Format::eR32G32B32A32Uint;
        case PixelFormatType::kR32G32_UINT:
            return vk::Format::eR32G32Uint;
        case PixelFormatType::kR32_UINT:
            return vk::Format::eR32Uint;
        case PixelFormatType::kUnknown:
            return vk::Format::eUndefined;
        default:
            mi_assert(false, "Unrecognized pixel format by the Vulkan backend. Missing transition code?");
            return vk::Format::eUndefined;
    }
}

FORCEINLINE PixelFormatType GetPixelFormatFromVulkanFormat (vk::Format format) {
    switch(format) {
        case vk::Format::eB8G8R8A8Unorm:
            return PixelFormatType::kB8G8R8A8_UNORM;
        case vk::Format::eB8G8R8A8Srgb:
            return PixelFormatType::kB8G8R8A8_SRGB;
        case vk::Format::eR8G8B8A8Unorm:
            return PixelFormatType::kR8G8B8A8_UNORM;
        case vk::Format::eR8G8B8A8Srgb:
            return PixelFormatType::kR8G8B8A8_SRGB;
        case vk::Format::eR16G16B16A16Sfloat:
            return PixelFormatType::kR16G16B16A16_FLOAT;
        case vk::Format::eR16G16Sfloat:
            return PixelFormatType::kR16G16_FLOAT;
        case vk::Format::eR32G32B32A32Sfloat:
            return PixelFormatType::kR32G32B32A32_FLOAT;
        case vk::Format::eR32G32B32Sfloat:
            return PixelFormatType::kR32G32B32_FLOAT;
        case vk::Format::eR32G32Sfloat:
            return PixelFormatType::kR32G32_FLOAT;
        case vk::Format::eR32Sfloat:
            return PixelFormatType::kR32_FLOAT;
        case vk::Format::eD32Sfloat:
            return PixelFormatType::kD32_FLOAT;
        case vk::Format::eR32G32B32A32Uint:
            return PixelFormatType::kR32G32B32A32_UINT;
        case vk::Format::eR32G32Uint:
            return PixelFormatType::kR32G32_UINT;
        case vk::Format::eR32Uint:
            return PixelFormatType::kR32_UINT;
        default:
            mi_assert(false, "Unrecognized pixel format by the Vulkan backend. Missing transition code?");
            return PixelFormatType::kUnknown;
    }
}

FORCEINLINE vk::ImageType GetVulkanImageType (RHITextureType type) {
    switch(type) {
        case RHITextureType::k2D:
            return vk::ImageType::e2D;
        case RHITextureType::k3D:
            return vk::ImageType::e3D;
        case RHITextureType::kCube:
            return vk::ImageType::e2D;
        case RHITextureType::k2DArray:
            return vk::ImageType::e2D;
        default:
            return vk::ImageType::e2D;
    }
}

FORCEINLINE vk::ImageViewType GetVulkanImageViewType (RHITextureType type) {
    switch(type) {
        case RHITextureType::k2D:
            return vk::ImageViewType::e2D;
        case RHITextureType::k3D:
            return vk::ImageViewType::e3D;
        case RHITextureType::kCube:
            return vk::ImageViewType::eCube;
        case RHITextureType::k2DArray:
            return vk::ImageViewType::e2DArray;
        default:
            return vk::ImageViewType::e2D;
    }
}

FORCEINLINE vk::ImageUsageFlags GetVulkanImageUsage (RHITextureUsageFlags usage) {
    vk::ImageUsageFlags vk_usage = {};
    if(usage & RHITextureUsageFlagBits::kDepthStencil) {
        vk_usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
    }
    if(usage & RHITextureUsageFlagBits::kRenderTarget) {
        vk_usage |= vk::ImageUsageFlagBits::eColorAttachment;
    }
    if(usage & RHITextureUsageFlagBits::kShaderResource) {
        vk_usage |= vk::ImageUsageFlagBits::eSampled;
    }
    if(usage & RHITextureUsageFlagBits::kUnorderedAccess) {
        vk_usage |= vk::ImageUsageFlagBits::eStorage;
    }
    if(usage & RHITextureUsageFlagBits::kTransferSrc) {
        vk_usage |= vk::ImageUsageFlagBits::eTransferSrc;
    }
    if(usage & RHITextureUsageFlagBits::kTransferDst) {
        vk_usage |= vk::ImageUsageFlagBits::eTransferDst;
    }
    return vk_usage;
}

FORCEINLINE vk::ImageAspectFlags GetVulkanImageAspectFlags (RHITextureUsageFlags usage) {
    vk::ImageAspectFlags vk_aspect = {};
    if(usage & RHITextureUsageFlagBits::kDepthStencil) {
        vk_aspect |= vk::ImageAspectFlagBits::eDepth;
    } else {
        vk_aspect |= vk::ImageAspectFlagBits::eColor;
    }
    return vk_aspect;
}

FORCEINLINE vk::Filter GetVulkanFilter (RHISamplerFilterType filter) {
    switch(filter) {
        case RHISamplerFilterType::kPoint:
            return vk::Filter::eNearest;
        case RHISamplerFilterType::kLinear:
            return vk::Filter::eLinear;
        default:
            return vk::Filter::eNearest;
    }
}

FORCEINLINE vk::SamplerMipmapMode GetVulkanMipmapMode (RHISamplerFilterType mipmap) {
    switch(mipmap) {
        case RHISamplerFilterType::kPoint:
            return vk::SamplerMipmapMode::eNearest;
        case RHISamplerFilterType::kLinear:
            return vk::SamplerMipmapMode::eLinear;
        default:
            return vk::SamplerMipmapMode::eNearest;
    }
}

FORCEINLINE vk::SamplerAddressMode GetVulkanAddressingMode (RHISamplerAddressModeType addressing) {
    switch(addressing) {
        case RHISamplerAddressModeType::kRepeat:
            return vk::SamplerAddressMode::eRepeat;
        case RHISamplerAddressModeType::kClampToEdge:
            return vk::SamplerAddressMode::eClampToEdge;
        case RHISamplerAddressModeType::kClampToBorder:
            return vk::SamplerAddressMode::eClampToBorder;
        default:
            return vk::SamplerAddressMode::eRepeat;
    }
}

FORCEINLINE vk::ShaderStageFlagBits GetVulkanShaderStage (RHIShaderFrequencyFlagBits frequency) {
    switch(frequency) {
        case RHIShaderFrequencyFlagBits::kVertex:
            return vk::ShaderStageFlagBits::eVertex;
        case RHIShaderFrequencyFlagBits::kFragment:
            return vk::ShaderStageFlagBits::eFragment;
        case RHIShaderFrequencyFlagBits::kGeometry:
            return vk::ShaderStageFlagBits::eGeometry;
        case RHIShaderFrequencyFlagBits::kTask:
            return vk::ShaderStageFlagBits::eTaskEXT;
        case RHIShaderFrequencyFlagBits::kMesh:
            return vk::ShaderStageFlagBits::eMeshEXT;
        case RHIShaderFrequencyFlagBits::kCompute:
            return vk::ShaderStageFlagBits::eCompute;
        case RHIShaderFrequencyFlagBits::kRaygen:
            return vk::ShaderStageFlagBits::eRaygenKHR;
        case RHIShaderFrequencyFlagBits::kMiss:
            return vk::ShaderStageFlagBits::eMissKHR;
        case RHIShaderFrequencyFlagBits::kClosestHit:
            return vk::ShaderStageFlagBits::eClosestHitKHR;
        case RHIShaderFrequencyFlagBits::kAnyHit:
            return vk::ShaderStageFlagBits::eAnyHitKHR;
        case RHIShaderFrequencyFlagBits::kIntersection:
            return vk::ShaderStageFlagBits::eIntersectionKHR;
        case RHIShaderFrequencyFlagBits::kCallable:
            return vk::ShaderStageFlagBits::eCallableKHR;
        default:
            mi_assert(false, "Invalid shader frequency");
            return {};
    }
}

FORCEINLINE vk::ShaderStageFlags GetVulkanShaderStageFlags (RHIShaderFrequencyFlags frequency) {
    vk::ShaderStageFlags flags = {};
    if(frequency & RHIShaderFrequencyFlagBits::kVertex) {
        flags |= vk::ShaderStageFlagBits::eVertex;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kFragment) {
        flags |= vk::ShaderStageFlagBits::eFragment;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kGeometry) {
        flags |= vk::ShaderStageFlagBits::eGeometry;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kTask) {
        flags |= vk::ShaderStageFlagBits::eTaskEXT;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kMesh) {
        flags |= vk::ShaderStageFlagBits::eMeshEXT;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kCompute) {
        flags |= vk::ShaderStageFlagBits::eCompute;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kRaygen) {
        flags |= vk::ShaderStageFlagBits::eRaygenKHR;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kMiss) {
        flags |= vk::ShaderStageFlagBits::eMissKHR;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kClosestHit) {
        flags |= vk::ShaderStageFlagBits::eClosestHitKHR;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kAnyHit) {
        flags |= vk::ShaderStageFlagBits::eAnyHitKHR;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kIntersection) {
        flags |= vk::ShaderStageFlagBits::eIntersectionKHR;
    }
    if(frequency & RHIShaderFrequencyFlagBits::kCallable) {
        flags |= vk::ShaderStageFlagBits::eCallableKHR;
    }
    return flags;
}

FORCEINLINE vk::VertexInputRate GetVulkanVertexInputRate (RHIVertexInputRateType type) {
    switch(type) {
        case RHIVertexInputRateType::kVertex:
            return vk::VertexInputRate::eVertex;
        case RHIVertexInputRateType::kInstance:
            return vk::VertexInputRate::eInstance;
        default:
            return vk::VertexInputRate::eVertex;
    }
}

FORCEINLINE vk::Format GetVulkanVertexAttributeFormat (RHIVertexAttributeFormatType type) {
    switch(type) {
        case RHIVertexAttributeFormatType::k1xFp32:
            return vk::Format::eR32Sfloat;
        case RHIVertexAttributeFormatType::k2xFp32:
            return vk::Format::eR32G32Sfloat;
        case RHIVertexAttributeFormatType::k3xFp32:
            return vk::Format::eR32G32B32Sfloat;
        case RHIVertexAttributeFormatType::k4xFp32:
            return vk::Format::eR32G32B32A32Sfloat;
        default:
            return vk::Format::eUndefined;
    }
}

FORCEINLINE RHIVertexAttributeFormatType GetRHIVertexAttributeFormat (vk::Format format) {
    switch(format) {
        case vk::Format::eR32Sfloat:
            return RHIVertexAttributeFormatType::k1xFp32;
        case vk::Format::eR32G32Sfloat:
            return RHIVertexAttributeFormatType::k2xFp32;
        case vk::Format::eR32G32B32Sfloat:
            return RHIVertexAttributeFormatType::k3xFp32;
        case vk::Format::eR32G32B32A32Sfloat:
            return RHIVertexAttributeFormatType::k4xFp32;
        default:
            return RHIVertexAttributeFormatType::kMax;
    }
}

FORCEINLINE vk::IndexType GetVulkanIndexType (RHIIndexType type) {
    switch(type) {
        case RHIIndexType::kUint16:
            return vk::IndexType::eUint16;
        case RHIIndexType::kUint32:
            return vk::IndexType::eUint32;
        default:
            return vk::IndexType::eUint32;
    }
}

FORCEINLINE vk::PrimitiveTopology GetVulkanPrimitiveTopology (RHIPrimitiveTopologyType type) {
    switch(type) {
        case RHIPrimitiveTopologyType::kPointList:
            return vk::PrimitiveTopology::ePointList;
        case RHIPrimitiveTopologyType::kLineList:
            return vk::PrimitiveTopology::eLineList;
        case RHIPrimitiveTopologyType::kLineStrip:
            return vk::PrimitiveTopology::eLineStrip;
        case RHIPrimitiveTopologyType::kTriangleList:
            return vk::PrimitiveTopology::eTriangleList;
        case RHIPrimitiveTopologyType::kTriangleStrip:
            return vk::PrimitiveTopology::eTriangleStrip;
        default:
            return vk::PrimitiveTopology::eTriangleList;
    }
}

FORCEINLINE vk::PolygonMode GetVulkanPolygonMode (RHIPolygonModeType mode) {
    switch(mode) {
        case RHIPolygonModeType::kFill:
            return vk::PolygonMode::eFill;
        case RHIPolygonModeType::kLine:
            return vk::PolygonMode::eLine;
        case RHIPolygonModeType::kPoint:
            return vk::PolygonMode::ePoint;
        default:
            return vk::PolygonMode::eFill;
    }
}

FORCEINLINE vk::CompareOp GetVulkanCompareOp (RHIDepthCompareOpType op) {
    switch(op) {
        case RHIDepthCompareOpType::kNever:
            return vk::CompareOp::eNever;
        case RHIDepthCompareOpType::kLess:
            return vk::CompareOp::eLess;
        case RHIDepthCompareOpType::kEqual:
            return vk::CompareOp::eEqual;
        case RHIDepthCompareOpType::kLessOrEqual:
            return vk::CompareOp::eLessOrEqual;
        case RHIDepthCompareOpType::kGreater:
            return vk::CompareOp::eGreater;
        case RHIDepthCompareOpType::kNotEqual:
            return vk::CompareOp::eNotEqual;
        case RHIDepthCompareOpType::kGreaterOrEqual:
            return vk::CompareOp::eGreaterOrEqual;
        case RHIDepthCompareOpType::kAlways:
            return vk::CompareOp::eAlways;
        default:
            return vk::CompareOp::eNever;
    }
}

FORCEINLINE vk::CullModeFlagBits GetVulkanCullMode (RHICullModeType mode) {
    switch(mode) {
        case RHICullModeType::kNone:
            return vk::CullModeFlagBits::eNone;
        case RHICullModeType::kFront:
            return vk::CullModeFlagBits::eFront;
        case RHICullModeType::kBack:
            return vk::CullModeFlagBits::eBack;
        default:
            return vk::CullModeFlagBits::eNone;
    }
}

FORCEINLINE vk::FrontFace GetVulkanFrontFace (RHIFrontFaceType face) {
    switch(face) {
        case RHIFrontFaceType::kClockwise:
            return vk::FrontFace::eClockwise;
        case RHIFrontFaceType::kCounterClockwise:
            return vk::FrontFace::eCounterClockwise;
        default:
            return vk::FrontFace::eCounterClockwise;
    }
}

FORCEINLINE vk::BlendFactor GetVulkanBlendFactor (RHIBlendFactorType factor) {
    switch(factor) {
        case RHIBlendFactorType::kZero:
            return vk::BlendFactor::eZero;
        case RHIBlendFactorType::kOne:
            return vk::BlendFactor::eOne;
        case RHIBlendFactorType::kSrcColor:
            return vk::BlendFactor::eSrcColor;
        case RHIBlendFactorType::kOneMinusSrcColor:
            return vk::BlendFactor::eOneMinusSrcColor;
        case RHIBlendFactorType::kDstColor:
            return vk::BlendFactor::eDstColor;
        case RHIBlendFactorType::kOneMinusDstColor:
            return vk::BlendFactor::eOneMinusDstColor;
        case RHIBlendFactorType::kSrcAlpha:
            return vk::BlendFactor::eSrcAlpha;
        case RHIBlendFactorType::kOneMinusSrcAlpha:
            return vk::BlendFactor::eOneMinusSrcAlpha;
        case RHIBlendFactorType::kDstAlpha:
            return vk::BlendFactor::eDstAlpha;
        case RHIBlendFactorType::kOneMinusDstAlpha:
            return vk::BlendFactor::eOneMinusDstAlpha;
        case RHIBlendFactorType::kConstantColor:
            return vk::BlendFactor::eConstantColor;
        case RHIBlendFactorType::kOneMinusConstantColor:
            return vk::BlendFactor::eOneMinusConstantColor;
        case RHIBlendFactorType::kConstantAlpha:
            return vk::BlendFactor::eConstantAlpha;
        case RHIBlendFactorType::kOneMinusConstantAlpha:
            return vk::BlendFactor::eOneMinusConstantAlpha;
        case RHIBlendFactorType::kSrcAlphaSaturate:
            return vk::BlendFactor::eSrcAlphaSaturate;
        case RHIBlendFactorType::kSrc1Color:
            return vk::BlendFactor::eSrc1Color;
        case RHIBlendFactorType::kOneMinusSrc1Color:
            return vk::BlendFactor::eOneMinusSrc1Color;
        case RHIBlendFactorType::kSrc1Alpha:
            return vk::BlendFactor::eSrc1Alpha;
        case RHIBlendFactorType::kOneMinusSrc1Alpha:
            return vk::BlendFactor::eOneMinusSrc1Alpha;
        default:
            return vk::BlendFactor::eZero;
    }
}

FORCEINLINE vk::BlendOp GetVulkanBlendOp (RHIBlendOpType op) {
    switch(op) {
        case RHIBlendOpType::kBlendAdd:
            return vk::BlendOp::eAdd;
        case RHIBlendOpType::kBlendSubtract:
            return vk::BlendOp::eSubtract;
        case RHIBlendOpType::kBlendReverseSubtract:
            return vk::BlendOp::eReverseSubtract;
        case RHIBlendOpType::kBlendMin:
            return vk::BlendOp::eMin;
        case RHIBlendOpType::kBlendMax:
            return vk::BlendOp::eMax;
        default:
            return vk::BlendOp::eAdd;
    }
}

FORCEINLINE vk::AttachmentLoadOp GetVulkanLoadOp (RHILoadOpType op) {
    switch(op) {
        case RHILoadOpType::kLoad:
            return vk::AttachmentLoadOp::eLoad;
        case RHILoadOpType::kClear:
            return vk::AttachmentLoadOp::eClear;
        case RHILoadOpType::kDontCare:
            return vk::AttachmentLoadOp::eDontCare;
        default:
            return vk::AttachmentLoadOp::eDontCare;
    }
}

FORCEINLINE vk::AttachmentStoreOp GetVulkanStoreOp (RHIStoreOpType op) {
    switch(op) {
        case RHIStoreOpType::kStore:
            return vk::AttachmentStoreOp::eStore;
        case RHIStoreOpType::kDontCare:
            return vk::AttachmentStoreOp::eDontCare;
        default:
            return vk::AttachmentStoreOp::eDontCare;
    }
}

FORCEINLINE vk::BufferUsageFlags GetVulkanBufferUsage (RHIBufferUsageFlags usage) {
    vk::BufferUsageFlags vk_usage = {};
    if(usage & RHIBufferUsageFlagBits::kVertex) {
        vk_usage |= vk::BufferUsageFlagBits::eVertexBuffer;
    }
    if(usage & RHIBufferUsageFlagBits::kIndex) {
        vk_usage |= vk::BufferUsageFlagBits::eIndexBuffer;
    }
    if(usage & RHIBufferUsageFlagBits::kUniform) {
        vk_usage |= vk::BufferUsageFlagBits::eUniformBuffer;
    }
    if(usage & RHIBufferUsageFlagBits::kStorage) {
        vk_usage |= vk::BufferUsageFlagBits::eStorageBuffer;
    }
    if(usage & RHIBufferUsageFlagBits::kIndirect) {
        vk_usage |= vk::BufferUsageFlagBits::eIndirectBuffer;
    }
    if(usage & RHIBufferUsageFlagBits::kReadback) {
        // vk_usage |= vk::BufferUsageFlagBits::;
    }
    if(usage & RHIBufferUsageFlagBits::kStaging) {
        vk_usage |= vk::BufferUsageFlagBits::eTransferSrc;
    }
    // Ray tracing specific buffer usages
    if(usage & RHIBufferUsageFlagBits::kAccelerationStructureStorage) {
        vk_usage |= vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR;
    }
    if(usage & RHIBufferUsageFlagBits::kAccelerationStructureBuildInput) {
        vk_usage |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
    }
    if(usage & RHIBufferUsageFlagBits::kShaderBindingTable) {
        vk_usage |= vk::BufferUsageFlagBits::eShaderBindingTableKHR;
    }
    if(usage & RHIBufferUsageFlagBits::kAccelerationStructureScratch) {
        vk_usage |= vk::BufferUsageFlagBits::eStorageBuffer; // Scratch buffers are storage buffers
    }

    if ((usage & RHIBufferUsageFlagBits::kStorage)
    || (usage & RHIBufferUsageFlagBits::kTransferSrc)) {
        vk_usage |= vk::BufferUsageFlagBits::eTransferSrc;
    }
    vk_usage |= vk::BufferUsageFlagBits::eTransferDst;

    // Ray tracing buffers need device address support
    if(usage & (RHIBufferUsageFlagBits::kAccelerationStructureStorage |
                RHIBufferUsageFlagBits::kAccelerationStructureBuildInput |
                RHIBufferUsageFlagBits::kShaderBindingTable |
                RHIBufferUsageFlagBits::kAccelerationStructureScratch |
                RHIBufferUsageFlagBits::kShaderDeviceAddress)) {
        vk_usage |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
    }

    return vk_usage;
}

FORCEINLINE vk::DescriptorType GetVulkanDescriptorType (RHIPipelineResourceType type) {
    switch (type) {
        case RHIPipelineResourceType::kUniformBuffer:
            return vk::DescriptorType::eUniformBuffer;
        case RHIPipelineResourceType::kStorageBuffer:
            return vk::DescriptorType::eStorageBuffer;
        case RHIPipelineResourceType::kUAV:
            return vk::DescriptorType::eStorageImage;
        case RHIPipelineResourceType::kSRV:
            return vk::DescriptorType::eSampledImage;
        case RHIPipelineResourceType::kSampler:
            return vk::DescriptorType::eSampler;
        case RHIPipelineResourceType::kAccelerationStructure:
            return vk::DescriptorType::eAccelerationStructureKHR;
        default:
            mi_assert(false, "Invalid pipeline resource type");
            return vk::DescriptorType::eUniformBuffer;
    }
}

FORCEINLINE RHIBindlessResourceType FromVulkanDescriptorType (vk::DescriptorType type) {
    switch (type) {
        case vk::DescriptorType::eStorageBuffer:
            return RHIBindlessResourceType::kReadOnlyStorageBuffer;
        case vk::DescriptorType::eSampledImage:
            return RHIBindlessResourceType::kSRV;
        case vk::DescriptorType::eAccelerationStructureKHR:
            return RHIBindlessResourceType::kAccelerationStructure;
        default:
            mi_assert(false, "Invalid pipeline resource type");
            return RHIBindlessResourceType::kMax;
    }
}

FORCEINLINE vk::ImageLayout GetVulkanImageLayout (RHITextureLayoutType type) {
    switch (type) {
        case RHITextureLayoutType::kUndefined:
            return vk::ImageLayout::eUndefined;
        case RHITextureLayoutType::kShaderReadOnlyOptimal:
            return vk::ImageLayout::eShaderReadOnlyOptimal;
        case RHITextureLayoutType::kColorAttachment:
            return vk::ImageLayout::eColorAttachmentOptimal;
        case RHITextureLayoutType::kDepthStencilAttachment:
            return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        case RHITextureLayoutType::kTransferSrcOptimal:
            return vk::ImageLayout::eTransferSrcOptimal;
        case RHITextureLayoutType::kTransferDstOptimal:
            return vk::ImageLayout::eTransferDstOptimal;
        case RHITextureLayoutType::kGeneral:
            return vk::ImageLayout::eGeneral;
        default:
            mi_assert(false, "Invalid texture layout type");
            return vk::ImageLayout::eUndefined;
    }
}

FORCEINLINE vk::AccessFlags2 GetVulkanAccessFlags (RHIGPUAccessFlags flags) {
    vk::AccessFlags2 vk_flags = {};
    // Special flags
    if (flags == RHIGPUAccessFlagBits::kAll || flags == RHIGPUAccessFlagBits::kRW) {
        return vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    }
    if ((flags & RHIGPUAccessFlagBits::kRead) == RHIGPUAccessFlagBits::kRead) {
        vk_flags |= vk::AccessFlagBits2::eMemoryRead;
        flags = flags & (~RHIGPUAccessFlagBits::kRead); // Clear read flag to avoid double counting
    }
    if((flags & RHIGPUAccessFlagBits::kWrite) == RHIGPUAccessFlagBits::kWrite) {
        vk_flags |= vk::AccessFlagBits2::eMemoryWrite;
        flags = flags & (~RHIGPUAccessFlagBits::kWrite); // Clear write flag to avoid double counting
    }
    if((flags & RHIGPUAccessFlagBits::kShaderRead) == RHIGPUAccessFlagBits::kShaderRead) {
        vk_flags |= vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eAccelerationStructureReadKHR;
        flags = flags & (~RHIGPUAccessFlagBits::kShaderRead); // Clear shader read flag to avoid double counting
    }
    // Ordinary flags
    if(flags & RHIGPUAccessFlagBits::kIndirectCommandRead) {
        vk_flags |= vk::AccessFlagBits2::eIndirectCommandRead;
    }
    if(flags & RHIGPUAccessFlagBits::kIndexRead) {
        vk_flags |= vk::AccessFlagBits2::eIndexRead;
    }
    if(flags & RHIGPUAccessFlagBits::kVertexAttributeRead) {
        vk_flags |= vk::AccessFlagBits2::eVertexAttributeRead;
    }
    if(flags & RHIGPUAccessFlagBits::kUniformRead) {
        vk_flags |= vk::AccessFlagBits2::eUniformRead;
    }
    if (flags & RHIGPUAccessFlagBits::kShaderStorageRead) {
        vk_flags |= vk::AccessFlagBits2::eShaderStorageRead;
    }
    if (flags & RHIGPUAccessFlagBits::kShaderSampledRead) {
        vk_flags |= vk::AccessFlagBits2::eShaderSampledRead;
    }
    if(flags & RHIGPUAccessFlagBits::kAccelerationStructureRead) {
        vk_flags |= vk::AccessFlagBits2::eAccelerationStructureReadKHR;
    }
    if(flags & RHIGPUAccessFlagBits::kTransferRead) {
        vk_flags |= vk::AccessFlagBits2::eTransferRead;
    }
    if(flags & RHIGPUAccessFlagBits::kDepthStencilRead) {
        vk_flags |= vk::AccessFlagBits2::eDepthStencilAttachmentRead;
    }
    if(flags & RHIGPUAccessFlagBits::kColorAttachmentRead) {
        vk_flags |= vk::AccessFlagBits2::eColorAttachmentRead;
    }
    if(flags & RHIGPUAccessFlagBits::kShaderBindingTableRead) {
        vk_flags |= vk::AccessFlagBits2::eShaderBindingTableReadKHR;
    }
    if(flags & RHIGPUAccessFlagBits::kShaderWrite) {
        vk_flags |= vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderStorageWrite;
    }
    if (flags & RHIGPUAccessFlagBits::kAccelerationStructureWrite) {
        vk_flags |= vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
    }
    if(flags & RHIGPUAccessFlagBits::kDepthStencilWrite) {
        vk_flags |= vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    }
    if(flags & RHIGPUAccessFlagBits::kColorAttachmentWrite) {
        vk_flags |= vk::AccessFlagBits2::eColorAttachmentWrite;
    }
    if(flags & RHIGPUAccessFlagBits::kTransferWrite) {
        vk_flags |= vk::AccessFlagBits2::eTransferWrite;
    }
    if (flags & RHIGPUAccessFlagBits::kShaderBindingTableRead) {
        vk_flags |= vk::AccessFlagBits2::eShaderBindingTableReadKHR;
    }
    return vk_flags;
}

FORCEINLINE vk::PipelineStageFlags2 GetVulkanPipelineStageFlags (RHIPipelineStageFlags stages) {
    vk::PipelineStageFlags2 vk_stages = {};
    // Special flags
    if(stages == RHIPipelineStageFlagBits::kAll) {
        return vk::PipelineStageFlagBits2::eAllCommands;
    }
    if (stages == RHIPipelineStageFlagBits::kAllGraphics) {
        return vk::PipelineStageFlagBits2::eAllGraphics;
    }
    // Ordinary flags
    if (stages & RHIPipelineStageFlagBits::kVertex) {
        vk_stages |= vk::PipelineStageFlagBits2::eVertexInput |
                vk::PipelineStageFlagBits2::eVertexShader |
                vk::PipelineStageFlagBits2::eIndexInput;
    }
    if (stages & RHIPipelineStageFlagBits::kGeometry) {
        vk_stages |= vk::PipelineStageFlagBits2::eGeometryShader;
    }
    if (stages & RHIPipelineStageFlagBits::kTess) {
        vk_stages |= vk::PipelineStageFlagBits2::eTessellationControlShader |
                vk::PipelineStageFlagBits2::eTessellationEvaluationShader;
    }
    if (stages & RHIPipelineStageFlagBits::kFragment) {
        vk_stages |= vk::PipelineStageFlagBits2::eFragmentShader |
                vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                vk::PipelineStageFlagBits2::eLateFragmentTests;
    }
    if(stages & RHIPipelineStageFlagBits::kFramebufferOutput) {
        vk_stages |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    }
    if (stages & RHIPipelineStageFlagBits::kTaskMesh) {
        vk_stages |= vk::PipelineStageFlagBits2::eTaskShaderEXT |
            vk::PipelineStageFlagBits2::eMeshShaderEXT;
    }
    if (stages & RHIPipelineStageFlagBits::kCompute) {
        vk_stages |= vk::PipelineStageFlagBits2::eComputeShader;
    }
    if (stages & RHIPipelineStageFlagBits::kAccelerationStructureBuild) {
        vk_stages |= vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
    }
    if (stages & RHIPipelineStageFlagBits::kRayTracing) {
        vk_stages |= vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
    }
    if (stages & RHIPipelineStageFlagBits::kTransfer) {
        vk_stages |= vk::PipelineStageFlagBits2::eTransfer
                   | vk::PipelineStageFlagBits2::eClear; // Include CLEAR for sync2 precise stages
    }
    if (stages & RHIPipelineStageFlagBits::kIndirect) {
        vk_stages |= vk::PipelineStageFlagBits2::eDrawIndirect;
    }
    return vk_stages;
}

FORCEINLINE vk::DescriptorType GetVulkanDescriptorType (RHIBindlessResourceType type) {
    switch (type) {
        case RHIBindlessResourceType::kReadOnlyStorageBuffer:
            return vk::DescriptorType::eStorageBuffer;
        case RHIBindlessResourceType::kSRV:
            return vk::DescriptorType::eSampledImage;
        case RHIBindlessResourceType::kAccelerationStructure:
            return vk::DescriptorType::eAccelerationStructureKHR;
        default:
            mi_assert(false, "Invalid pipeline resource type");
            return {};
    }
}

FORCEINLINE vk::BuildAccelerationStructureFlagsKHR GetVulkanBuildAccelerationStructureFlags (RHIAccelerationStructureBuildFlags flags) {
    vk::BuildAccelerationStructureFlagsKHR ret = {};
    if (flags & RHIAccelerationStructureBuildFlagBits::kAllowUpdate) {
        ret |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
    }
    if (flags & RHIAccelerationStructureBuildFlagBits::kAllowCompaction) {
        ret |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction;
    }
    if (flags & RHIAccelerationStructureBuildFlagBits::kPreferFastTrace) {
        ret |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    }
    if (flags & RHIAccelerationStructureBuildFlagBits::kPreferFastBuild) {
        ret |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
    }
    if (flags & RHIAccelerationStructureBuildFlagBits::kLowMemory) {
        ret |= vk::BuildAccelerationStructureFlagBitsKHR::eLowMemory;
    }
    return ret;
}

FORCEINLINE vk::GeometryFlagsKHR GetVulkanGeometryFlags (RHIASGeometryFlags flags) {
    vk::GeometryFlagsKHR ret = {};
    if (flags & RHIASGeometryFlagBits::kOpaque) {
        ret |= vk::GeometryFlagBitsKHR::eOpaque;
    }
    if (flags & RHIASGeometryFlagBits::kNoDuplicateAnyHitInvocation) {
        ret |= vk::GeometryFlagBitsKHR::eNoDuplicateAnyHitInvocation;
    }
    return ret;
}

FORCEINLINE vk::GeometryInstanceFlagsKHR GetVulkanAccelerationStructureInstanceFlags (RHIASGeometryInstanceFlags flags) {
    auto ret = vk::GeometryInstanceFlagsKHR{};
    if (flags & RHIASGeometryInstanceFlagBits::kDisableTriangleFaceCulling) {
        ret |= vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable;
    }
    if (flags & RHIASGeometryInstanceFlagBits::kFlipTriangleFacing) {
        ret |= vk::GeometryInstanceFlagBitsKHR::eTriangleFrontCounterclockwise;
    }
    if (flags & RHIASGeometryInstanceFlagBits::kForceOpaque) {
        ret |= vk::GeometryInstanceFlagBitsKHR::eForceOpaque;
    }
    if (flags & RHIASGeometryInstanceFlagBits::kForceNoOpaque) {
        ret |= vk::GeometryInstanceFlagBitsKHR::eForceNoOpaque;
    }
    return ret;
}

MI_NAMESPACE_END

#endif //MI_VK_CONVERSION_H
