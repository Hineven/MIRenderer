/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_FWD_H
#define MIRENDERERDEV_RHI_FWD_H

#include "core/refcounted.h"
#include "rhi/rhi_common.h"

MI_NAMESPACE_BEGIN

class RHIResource;
class RHIBuffer;
class RHITexture;
class RHISampler;
class RHIFrameBuffer;

// These references can only be used within the render thread
using RHIResourceRef = TRef<RHIResource>;
using RHIBufferRef = TRef<RHIBuffer>;
using RHITextureRef = TRef<RHITexture>;
using RHISamplerRef = TRef<RHISampler>;
using RHIFrameBufferRef = TRef<RHIFrameBuffer>;


class RHIShader;

using RHIShaderRef = TRef<RHIShader>;

class RHIBindPipelineParametersDesc;

class RHIGraphicsPipeline;
class RHIRayTracingPipeline;
class RHIComputePipeline;

using RHIGraphicsPipelineRef = TRef<RHIGraphicsPipeline>;
using RHIRayTracingPipelineRef = TRef<RHIRayTracingPipeline>;
using RHIComputePipelineRef = TRef<RHIComputePipeline>;

class RHICommandBase;
class RHICommandQueueBase;
class RHICommandQueueGraphics;

// This class is used internally, so it's definition is not exposed to the user
class RHICommandExecutor;


// Forward declaration
class RHIBuffer;
struct RHIBufferSpan;
class RHITexture;
class RHISampler;
class RHIAccelerationStructure;

class RHICommandQueueGraphics;
class RHIBindlessManager;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_FWD_H
