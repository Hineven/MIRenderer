/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MATERIAL_H
#define MI_MATERIAL_H

#include <string>
#include <glm/vec3.hpp>

#include <core/common.h>
#include <core/base.h>
#include <core/refcounted.h>
#include <rhi/rhi_cmd.h>
#include <renderer/mi_dirty_tracker.h>
#include <renderer/mi_renderer_fwd.h>
#include "../../shaders/shared/SharedMaterial.hlsl"

// Simple material implementation. Only uber material supported
MI_NAMESPACE_BEGIN

enum class MaterialFlagBits : unsigned {
    kNone = 0,
    kPointSampled = 1 << 0, // Point sampled texture
    kForward = 1 << 1, // Simple forward material
    kDoubleSided = 1 << 2, // Double-sided material
};

MAKE_FLAGS(Material)

class DeviceMaterial : public NonMovable, public RefCounted<> {
public:
    friend class Material;
    FORCEINLINE uint32_t GetIndex () const {return index_;}
    FORCEINLINE bool IsValid () const {
        return index_ != UINT32_MAX;
    }
protected:
    DeviceMaterial (DeviceBindlessResourceAllocator * allocator);
    ~DeviceMaterial () ;

    DeviceBindlessResourceAllocator * allocator_ {};

    // Index of the material (assigned by the renderer)
    uint32_t index_ {UINT32_MAX};
    MaterialHeader material_header_;
};

class Material : public NonMovable, public RefCounted<> {
public:
    friend class DeviceMaterial;

    void SetAlbedoTexture(Texture * texture);
    void SetNormalTexture(Texture * texture);
    void SetMetallicRoughnessTexture(Texture * texture);
    void SetEmissiveTexture(Texture * texture);

    FORCEINLINE void SetAlbedo (glm::vec4 albedo) {
        if (albedo_ != albedo) SetDirty();
        albedo_ = albedo;
    }

    FORCEINLINE void SetRoughness (float roughness) {
        if (roughness_ != roughness) SetDirty();
        roughness_ = roughness;
    }
    FORCEINLINE void SetMetallic (float metallic) {
        if (metallic != metallic_) SetDirty();
        metallic_ = metallic;
    }

    FORCEINLINE void SetEmissive (glm::vec3 emissive) {
        if (emissive_ != emissive) SetDirty();
        emissive_ = emissive;
    }
    FORCEINLINE bool IsEmissive () const {
        return emissive_texture_.IsValid() || glm::length(emissive_) > 0.f;
    }

    FORCEINLINE void SetDoubleSided (bool double_sided) {
        if (IsDoubleSided() != double_sided) SetDirty();
        if (double_sided) flags_ |= MaterialFlagBits::kDoubleSided;
        else flags_ = flags_ & MaterialFlags(~double_sided);
    }

    FORCEINLINE bool IsDoubleSided () const {
        return bool(flags_ & MaterialFlagBits::kDoubleSided);
    }

    FORCEINLINE void SetFlags (MaterialFlags flags) {
        if (flags_ != flags) SetDirty();
        flags_ = flags;
    }

    FORCEINLINE MaterialFlags GetFlags () const {
        return flags_;
    }

    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }
    FORCEINLINE void SetDirty (bool dirty = true) {
        if (tracker_ && !dirty_ && dirty) {
            tracker_->OnObjectTurnedDirty(this);
        }
        dirty_ = dirty;
    }

    FORCEINLINE const std::string & GetName () const {
        return name_;
    }

    MaterialHeader PackMaterialHeader () const ;

    void UpdateOnDevice_Async (DeviceBindlessResourceAllocator * allocator, RHICommandQueueGraphics & queue) ;
    void UpdateOnDevice (DeviceBindlessResourceAllocator * allocator) ;

    DeviceMaterial * GetDeviceMaterial () {return device_material_.Raw();}

    FORCEINLINE static TRef<Material> Create (
        glm::vec4 albedo = {1.f, 1.f, 1.f, 1.f},
        float roughness = 0.5f,
        glm::vec3 emissive = {0.f, 0.f, 0.f}
    ) {
        return Create("unnamed", albedo, roughness, emissive);
    }

    FORCEINLINE bool IsOpaque () const {
        return opaque_;
    }

    FORCEINLINE void SetOpaque (bool opaque) {
        if (opaque != opaque_) SetDirty(true);
        opaque_ = opaque;
    }

    static TRef<Material> Create (
        std::string name,
        glm::vec4 albedo = {1.f, 1.f, 1.f, 1.f},
        float roughness = 0.5f,
        glm::vec3 emissive = {0.f, 0.f, 0.f}
    );

    FORCEINLINE bool IsForward () const {
        return bool(flags_ & MaterialFlagBits::kForward) ;
    }

    FORCEINLINE void SetForward (bool forward) {
        if (IsForward() != forward) SetDirty();
        if (forward) flags_ |= MaterialFlagBits::kForward;
        else flags_ = flags_ & MaterialFlags(~MaterialFlagBits::kForward);
    }

protected:

    Material();
    ~Material();

    std::string name_ {};

    MaterialFlags flags_ {};

    glm::vec4 albedo_ {1.f};
    float roughness_ {0.5f};
    float metallic_ {0.f};
    glm::vec3 emissive_ {0.f};
    TRef<Texture> albedo_texture_;
    TRef<Texture> normal_texture_;
    TRef<Texture> metallic_roughness_texture_;
    TRef<Texture> emissive_texture_;

    // Opaque materials should always have alpha channel equals to 1.0f (not translucent)
    bool opaque_ {false};


    DirtyTracker<Material> * tracker_ {};

    bool dirty_ {true};

    TRef<DeviceMaterial> device_material_;
};

MI_NAMESPACE_END
#endif //MI_MATERIAL_H

