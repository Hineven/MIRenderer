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

class RHISyncPoint;

class RHIResource;
class RHIBuffer;
class RHITexture;
class RHISampler;
class RHISyncPoint;
class RHITimestamp; // Added forward declaration for timestamp resource

// These references can only be used within the render thread
using RHIResourceRef = TRef<RHIResource>;
using RHIBufferRef = TRef<RHIBuffer>;
using RHITextureRef = TRef<RHITexture>;
using RHISamplerRef = TRef<RHISampler>;
using RHISyncPointRef = TRef<RHISyncPoint>;
using RHITimestampRef = TRef<RHITimestamp>; // Added timestamp ref type


class RHIShader;

using RHIShaderRef = TRef<RHIShader>;

struct RHIBindPipelineParametersDesc;

class RHIGraphicsPipeline;
class RHIRayTracingPipeline;
class RHIComputePipeline;

using RHIGraphicsPipelineRef = TRef<RHIGraphicsPipeline>;
using RHIRayTracingPipelineRef = TRef<RHIRayTracingPipeline>;
using RHIComputePipelineRef = TRef<RHIComputePipeline>;

class RHICommandBase;
class RHICommandQueueBase;
class RHICommandQueueGraphics;

class RHICommandExecutorInterface;


// Forward declaration
class RHIBuffer;
struct RHIBufferSpan;
class RHITexture;
class RHISampler;
class RHIAccelerationStructure;

class RHICommandQueueGraphics;
class RHIBindlessManager;
template<typename T>
class RHIBindlessSlotKeeper;

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_FWD_H
