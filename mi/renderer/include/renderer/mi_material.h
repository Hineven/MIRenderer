/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MATERIAL_H
#define MI_MATERIAL_H

// Simple material implementation. Only uber material supported
#include <glm/vec3.hpp>
#include "core/common.h"
#include "core/base.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "renderer/mi_renderer_fwd.h"

MI_NAMESPACE_BEGIN

// Bindless texture the renderer uses.
class BindlessRendererTexture : public NonMovable, public RefCounted<> {
protected:
    uint32_t index_ {UINT32_MAX};
    RenderResourceAllocator * allocator_;
public:
    FORCEINLINE int GetIndex () const {return index_;}
    FORCEINLINE bool IsValid () const {return index_ != UINT32_MAX;}
    void Set (RHITexture * texture) ;
protected:
    BindlessRendererTexture (RenderResourceAllocator * allocator) ;
    ~BindlessRendererTexture () ;
};

class Material : public NonMovable, public RefCounted<> {
public:
    friend class Renderer;
    FORCEINLINE uint32_t GetIndex () const {return index_;}
protected:
    Material (RenderResourceAllocator * allocator);
    ~Material () ;

    RenderResourceAllocator * allocator_ {};

    // Index of the material (assigned by the renderer)
    uint32_t index_ {UINT32_MAX};
    MinimumMaterial minimum_material_;

    // Keep references to the textures used by this material.
    TRef<BindlessRendererTexture> albedo_texture_;
    TRef<BindlessRendererTexture> normal_texture_;
    TRef<BindlessRendererTexture> roughness_texture_;
};

MI_NAMESPACE_END
#endif //MI_MATERIAL_H
