/*
 * Created: 2025/4/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_FWD_H
#define RDG_FWD_H

#include <functional>
#include "core/common.h"
#include "core/refcounted.h"
#include "core/types.h"
MI_NAMESPACE_BEGIN

enum class RDGPassFlagBits : unsigned {
    kNone = 0,
    // Do not cull this pass when compiling the graph
    kNeverCull = 1 << 0,
    kAll = 0xffffffffu
};

MAKE_FLAGS(RDGPass);

enum class RDGResourceFlagBits : unsigned {
    kNone = 0,
    // The resource is used for exportation (for presentation, etc.), making passes writing to it never culled.
    kExport = 1 << 0,
    // Whether the resource is imported from external RHI resource. (thus should not be related to the pool)
    // Imported resources are also created with kExport flag.
    kImported = 1 << 1
};

MAKE_FLAGS(RDGResource);

class RDGResource;
typedef TRef<RDGResource> RDGResourceRef;

class RDGBuffer;
class RDGTexture;


enum class RDGPassType {
    // Invoking draw commands
    kGraphics,
    // Compute shader
    kCompute,
    // Ray tracing pass, using ray tracing shaders
    kRayTracing,
    // Custom pass, executing whatever you want
    kGeneric,
    kMax
};

// Describe how a RDG texture is used in a pass. Used for automatic barrier placement.
enum class RDGTextureUsageType {
    kNone = 0,
    // kTransferSrc = 1 << 0, // Transfer source, read-only
    // kTransferDst = 1 << 1, // Transfer destination, write-only
    kShaderRead, // Shader read, read-only
    kShaderReadWrite, // Shader read/write, read-write
    kOutputAttachment, // Output attachment, read-write
    kOverwriteOutputAttachment, // Output attachment, write only
    kDepthStencilAttachment, // Depth/stencil attachment, read-write
    kReadonlyDepthStencilAttachment, // Depth/stencil attachment, read-only
    kTransferRead, // Transfer read, for copy operations, read-only
    kTransferWrite, // Transfer write, for copy/clear operations, write-only
    kMax // Maximum value, used for validation
};

class RDGPass;
class RHICommandQueueGraphics;
typedef std::function<void(RDGPass*, RHICommandQueueGraphics&)> RDGPassLambda;

class RenderGraphBuilder;

// Some pointer-based shader parameters that can be set to null are initialized to this value
// to indicate that they are not set by the user.
// This is used to check if the user has set the parameter (setting to nullptr also counts).
constexpr static uint64_t RDGParameter_UnsetPointer = 0xffffffffffffffffull;
template<typename T> concept CPointerType = std::is_pointer_v<T>;

// Check if the render thread is in a rdg pass lambda.
bool RDG_IsInRDGExecution () ;

MI_NAMESPACE_END

#endif //RDG_FWD_H
