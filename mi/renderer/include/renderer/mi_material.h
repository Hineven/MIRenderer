/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MATERIAL_H
#define MI_MATERIAL_H

#include <string>
#include <glm/vec3.hpp>
#include "core/common.h"
#include "core/base.h"
#include "core/refcounted.h"
#include "renderer/mi_renderer_fwd.h"
#include "../../shaders/shared/SharedMaterial.hlsl"

// Simple material implementation. Only uber material supported
MI_NAMESPACE_BEGIN


class DeviceMaterial : public NonMovable, public RefCounted<> {
public:
    friend class Material;
    FORCEINLINE uint32_t GetIndex () const {return index_;}
protected:
    DeviceMaterial (CommonGroupedDeviceResourceAllocator * allocator);
    ~DeviceMaterial () ;

    CommonGroupedDeviceResourceAllocator * allocator_ {};

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

    MaterialHeader PackMaterialHeader () const ;

    void UpdateOnDevice (CommonGroupedDeviceResourceAllocator * allocator) ;

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

    Material();
    ~Material();

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

    bool dirty_ {true};

    TRef<DeviceMaterial> device_material_;
};

MI_NAMESPACE_END
#endif //MI_MATERIAL_H

