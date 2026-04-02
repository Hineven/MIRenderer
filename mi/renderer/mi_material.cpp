/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_material.h"

#include <rhi/rhi.h>
#include <rhi/rhi_bindless.h>
#include <rdg/rdg_helper.h>

#include <renderer/mi_renderer.h>
#include <renderer/mi_texture.h>
#include <renderer/mi_resource_allocator.h>

MI_NAMESPACE_BEGIN

DeviceMaterial::DeviceMaterial(DeviceBindlessResourceAllocator *allocator) {
    allocator_ = allocator;
    slot_ = allocator_->AllocateMaterialSlotKeeper();
}

DeviceMaterial::~DeviceMaterial() {
    // Slot is freed (delayed) by SlotKeeper.
}

Material::Material() {

}

Material::~Material() {

}

void Material::SetAlbedoTexture(Texture * texture) {
    albedo_texture_ = texture;
    dirty_ = true;
}

void Material::SetNormalTexture(Texture * texture) {
    normal_texture_ = texture;
    dirty_ = true;
}

void Material::SetMetallicRoughnessTexture(Texture * texture) {
    metallic_roughness_texture_ = texture;
    dirty_ = true;
}

void Material::SetEmissiveTexture(Texture * texture) {
    emissive_texture_ = texture;
    dirty_ = true;
}

MaterialHeader Material::PackMaterialHeader() const {
    MaterialHeader header = {};
    header.Albedo = albedo_;
    header.Flags = flags_;
    header.Emissive = emissive_;
    header.Roughness = roughness_;
    header.Metallic = metallic_;
    header.SpecularTint = glm::vec3{1.f};
    header.AlbedoMap = albedo_texture_ ? albedo_texture_->GetBindlessIndex() : UINT32_MAX;
    header.NormalMap = normal_texture_ ? normal_texture_->GetBindlessIndex() : UINT32_MAX;
    header.EmissiveMap = emissive_texture_ ? emissive_texture_->GetBindlessIndex() : UINT32_MAX;
    header.MetallicRoughnessMap = metallic_roughness_texture_ ? metallic_roughness_texture_->GetBindlessIndex() : UINT32_MAX;
    return header;
}

void Material::UpdateOnDevice_Async(DeviceBindlessResourceAllocator *allocator, RHICommandQueueGraphics &queue) {
    if (dirty_) {
        if (!device_material_) {
            device_material_ = new DeviceMaterial(allocator);
            mi_check(device_material_->IsValid(), "Failed to allocate device material slot. This may indicate that the device allocator is full.");
        }
        auto idx = device_material_->GetIndex();
        assert(idx != UINT32_MAX);
        device_material_->material_header_ = PackMaterialHeader();
        Helpers::Upload_Async(queue,
            allocator->GetMaterialHeaderBuffer(), sizeof(MaterialHeader) * idx,
            device_material_->material_header_
        );
        dirty_ = false;
    }
}


void Material::UpdateOnDevice(DeviceBindlessResourceAllocator *allocator) {
    UpdateOnDevice_Async(allocator, RHI::Get().GetGraphicsCommandQueue());
    RHI::Get().GetGraphicsCommandQueue().WaitForIdle("Material::UpdateOnDevice " + GetName());
}

TRef<Material> Material::Create(std::string name, glm::vec4 albedo, float roughness, glm::vec3 emissive) {
    auto material = new Material();
    material->SetAlbedo(albedo);
    material->SetRoughness(roughness);
    material->SetEmissive(emissive);
    material->name_  = name;
    material->dirty_ = true;
    return {material};
}



MI_NAMESPACE_END
