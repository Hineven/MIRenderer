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
    // The resource may be used among multiple RDGs, making passes writing to it never be culled
    kPersistent = 1 << 0,
    // Whether the resource is imported from external RHI resource. (thus should not be related to the pool)
    // Imported resources are also created with persistent flag.
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
    // Basic RHI commands
    kGeneric,
    // TODO add more (mesh, raytracing, etc)
    kMax
};

enum class RDGTextureUsageType : uint32_t {
    kNone = 0,
    // No usage specified, barrier all previous operations and discard the contents
    kDontCare,
    kTransferDst,
    kTransferSrc,
    kShaderRead,
    // Storage image
    kShaderReadWrite,
    kOutputAttachment,
    kDepthStencilAttachment,
    kMax
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

MI_NAMESPACE_END

#endif //RDG_FWD_H
