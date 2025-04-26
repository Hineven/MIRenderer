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
#include "rhi/rhi_bindlesskeeper.h"
#include "renderer/mi_renderer_fwd.h"

MI_NAMESPACE_BEGIN

// Bindless texture the renderer uses.
class BindlessRendererTexture : public NonMovable, public RefCounted<> {
protected:
    uint32_t index_ {UINT32_MAX};
    RenderResourceAllocator * allocator_;
    TRef<RHIBindlessSlotKeeper<RHITexture>> keeper_;
public:
    // Get the index of the texture in the resource allocator
    FORCEINLINE int GetIndex () const {return index_;}
    // Get the bindless index (RHI layer) of the texture, which is used to access it on the GPU.
    FORCEINLINE int GetBindlessIndex () const {return keeper_->GetSlot();}
    FORCEINLINE bool IsValid () const {return index_ != UINT32_MAX;}
    void Set (RHITexture * texture) ;
protected:
    BindlessRendererTexture (RenderResourceAllocator * allocator) ;
    ~BindlessRendererTexture () ;
};

class DeviceMaterial : public NonMovable, public RefCounted<> {
public:
    friend class Renderer;
    FORCEINLINE uint32_t GetIndex () const {return index_;}
    FORCEINLINE void SetAlbedoTexture (RHITexture * texture) {
        albedo_texture_->Set(texture);
        if (texture) minimum_material_.albedo_map_ = albedo_texture_->GetBindlessIndex();
        else minimum_material_.albedo_map_ = UINT32_MAX;
    }
    FORCEINLINE void SetNormalTexture (RHITexture * texture) {
        normal_texture_->Set(texture);
        if (normal_texture_) minimum_material_.normal_map_ = normal_texture_->GetBindlessIndex();
        else minimum_material_.normal_map_ = UINT32_MAX;
    }
protected:
    DeviceMaterial (RenderResourceAllocator * allocator);
    ~DeviceMaterial () ;

    RenderResourceAllocator * allocator_ {};

    // Index of the material (assigned by the renderer)
    uint32_t index_ {UINT32_MAX};
    MinimumMaterial minimum_material_;

    // Keep references to the textures used by this material.
    TRef<BindlessRendererTexture> albedo_texture_;
    TRef<BindlessRendererTexture> normal_texture_;
    TRef<BindlessRendererTexture> roughness_texture_;
};

class Material : public NonMovable, public RefCounted<> {
public:
    friend class DeviceMaterial;

    FORCEINLINE void SetAlbedoTexture (Texture * texture) {
        albedo_texture_ = texture;
        dirty_ = true;
    }
    FORCEINLINE void SetNormalTexture (Texture * texture) {
        normal_texture_ = texture;
        dirty_ = true;
    }

    FORCEINLINE void SetMetallicRoughnessTexture (Texture * texture) {
        metallic_roughness_texture_ = texture;
        dirty_ = true;
    }

    FORCEINLINE void SetEmissiveTexture (Texture * texture) {
        emissive_texture_ = texture;
        dirty_ = true;
    }

    FORCEINLINE void SetAlbedo (glm::vec4 albedo) {
        albedo_ = albedo;
        dirty_ = true;
    }

    FORCEINLINE void SetRoughness (float roughness) {
        roughness_ = roughness;
        dirty_ = true;
    }
    FORCEINLINE void SetMetallic (float metallic) {
        metallic_ = metallic;
        dirty_ = true;
    }

    FORCEINLINE void SetEmissive (glm::vec3 emissive) {
        emissive_ = emissive;
        dirty_ = true;
    }

    FORCEINLINE void SetDoubleSided (bool double_sided) {
        double_sided_ = double_sided;
        dirty_ = true;
    }

    FORCEINLINE bool IsDoubleSided () const {
        return double_sided_;
    }

    void CreateOnDevice (RenderResourceAllocator * allocator) ;

    DeviceMaterial * GetDeviceMaterial () {return device_material_.Raw();}

    FORCEINLINE static TRef<Material> Create (
        glm::vec4 albedo = {1.f, 1.f, 1.f, 1.f},
        float roughness = 0.5f,
        glm::vec3 emissive = {0.f, 0.f, 0.f}
    ) {
        return Create("unnamed", albedo, roughness, emissive);
    }


    static TRef<Material> Create (
        std::string name,
        glm::vec4 albedo = {1.f, 1.f, 1.f, 1.f},
        float roughness = 0.5f,
        glm::vec3 emissive = {0.f, 0.f, 0.f}
    );

protected:

    std::string name_ {};

    glm::vec4 albedo_ {1.f};
    float roughness_ {0.5f};
    float metallic_ {0.f};
    glm::vec3 emissive_ {0.f};
    TRef<Texture> albedo_texture_;
    TRef<Texture> normal_texture_;
    TRef<Texture> metallic_roughness_texture_;
    TRef<Texture> emissive_texture_;

    bool double_sided_ {false};

    bool dirty_ {false};

    TRef<DeviceMaterial> device_material_;
};

MI_NAMESPACE_END
#endif //MI_MATERIAL_H
