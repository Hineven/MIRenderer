/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_material.h"

#include <renderer/mi_renderer.h>
#include <renderer/mi_texture.h>

#include <rhi/rhi_bindless.h>
#include "renderer/mi_resource_allocator.h"
#include "rhi/rhi.h"
#include "rhi/rhi_texture.h"
MI_NAMESPACE_BEGIN


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

MI_NAMESPACE_END
