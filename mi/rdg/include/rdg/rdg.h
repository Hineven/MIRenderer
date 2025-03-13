/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_H
#define RDG_H

#include "rdg_base.h"
#include "rdg_pass.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN
class RDGShader;

class RenderGraph : public RefCounted<> {
protected:
    void Execute (RenderResourcePool & pool) ;
    void ExecutePass (RDGPass & pass) ;
public:
    friend class RDGCommands;
};

typedef TRef<RenderGraph> RenderGraphRef;

MI_NAMESPACE_END

#endif //RDG_H
