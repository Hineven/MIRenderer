/*
 * Created: 2025/2/28
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_PASS_H
#define RDG_PASS_H
#include "rdg/rdg_base.h"
MI_NAMESPACE_BEGIN

class RDGPass : public NonMovable, public NonCopyable{
protected:
    // Can only be allocated by RDG
    RDGPass() = default;
public:
    virtual ~RDGPass() = default;
    virtual void Execute (RenderResourcePool & pool) = 0;
protected:
    template<typename T>
    struct RDGResourceAccess {
        TRef<T> resource;
        RHIGPUAccessFlags access;
    };
    typedef RDGResourceAccess<RDGTexture> RDGTextureAccess;
    typedef RDGResourceAccess<RDGBuffer> RDGBufferAccess;

    std::vector<RDGTextureAccess> accessed_textures_;
    std::vector<RDGBufferAccess> accessed_buffers_;
    // std::vector<RDGResourceAccess> accessed_acceleration_structures_;

    std::function<void()> pass_;
};

MI_NAMESPACE_END

#endif //RDG_PASS_H
