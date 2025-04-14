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
#include "mi_renderer_fwd.h"
#include "core/refcounted.h"
MI_NAMESPACE_BEGIN
class RHITexture;


// Bindless texture the renderer uses.
class BindlessRendererTexture : RefCounted<> {
protected:
    TRef<RHITexture> texture_ {};
    uint32_t index_ {UINT32_MAX};
public:
    FORCEINLINE int GetIndex () const {return index_;}
    FORCEINLINE bool IsValid () const {return texture_ != nullptr && index_ != UINT32_MAX;}
    void Set (TRef<RHITexture> texture) ;
    BindlessRendererTexture ();
    ~BindlessRendererTexture () ;
};

class Material {
public:
    friend class Renderer;
    FORCEINLINE uint32_t GetIndex () const {return index_;}
protected:
    Material ();
    ~Material () ;

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
