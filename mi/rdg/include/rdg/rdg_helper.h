/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_HELPER_H
#define RDG_HELPER_H

#include "rdg/rdg.h"
MI_NAMESPACE_BEGIN

// Simple helpers for easily adding commonly used RDG passes.
class RDGHelper {
public:
    // Spawn a pass that creates a dispatch indirect command with the specified number of thread groups.
    static TRef<RDGBuffer> SpawnDispatchIndirectCommand1D (RenderGraphBuilder & builder, RDGBuffer * count_buffer);
};

MI_NAMESPACE_END

#endif //RDG_HELPER_H
