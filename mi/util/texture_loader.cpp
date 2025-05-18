/*
 * Created: 2025/5/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <fstream>
#include <util/texture_loader.h>


#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "core/infra.h"
#include "rdg/rdg.h"
#include "rdg/rdg_shader.h"
#include "renderer/mi_texture.h"

MI_NAMESPACE_BEGIN

class MappingShader : public RDGShader {
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_PARAMETER(float4x4, ViewProjectionInverse)
        SHADER_PARAMETER(Texture2D, InEnvironmentMap)
        SHADER_PARAMETER(RWTexture2D, OutEnvironmentMap)
        SHADER_PARAMETER(uint2, TextureDimensions)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params);
};

TRef<Texture> TextureLoader::LoadEnvironmentMapFromBuffer(const std::string &name, const std::string &mime_type, const void *ptr, size_t size) {
    auto env_texture = LoadFromBuffer(name + "_env", mime_type, ptr, size);
    env_texture->CreateOnDevice();

    glm::dvec3 const forward_vectors[] = {glm::dvec3(-1.0, 0.0, 0.0), glm::dvec3(1.0, 0.0, 0.0),
    glm::dvec3(0.0, 1.0, 0.0), glm::dvec3(0.0, -1.0, 0.0), glm::dvec3(0.0, 0.0, -1.0),
    glm::dvec3(0.0, 0.0, 1.0)};

    glm::dvec3 const up_vectors[] = {glm::dvec3(0.0, -1.0, 0.0), glm::dvec3(0.0, -1.0, 0.0),
        glm::dvec3(0.0, 0.0, -1.0), glm::dvec3(0.0, 0.0, 1.0), glm::dvec3(0.0, -1.0, 0.0),
        glm::dvec3(0.0, -1.0, 0.0)};

    auto mapping_shader = RDGShaderLibrary::Get().GetShader<MappingShader>();
    MappingShader::ShaderParameters params;
    // 1k res
    params.TextureDimensions = {1024, 1024};
    params.InEnvironmentMap = env_texture;
    params.OutEnvironmentMap = env_texture;

    gfxProgramSetParameter(gfx, ibl_program_, "g_BufferDimensions", buffer_dimensions);
    gfxProgramSetParameter(gfx, ibl_program_, "g_EnvironmentMap", in_environment_texture);
    gfxProgramSetParameter(gfx, ibl_program_, "g_LinearSampler", AppInternal::GetInstance().GetSamplers().linear_wrap);

    for (uint32_t cubemap_face = 0; cubemap_face < 6; ++cubemap_face)
    {

        gfxCommandBindColorTarget(gfx, 0, environment_map_, 0, cubemap_face);

        glm::dmat4 const view =
            glm::lookAt(glm::dvec3(0.0), forward_vectors[cubemap_face], up_vectors[cubemap_face]);
        glm::dmat4 const proj          = glm::perspective(M_PI / 2.0, 1.0, 0.1, 1e4);
        glm::mat4 const  view_proj_inv = glm::mat4(glm::inverse(proj * view));

        gfxProgramSetParameter(gfx, ibl_program_, "g_ViewProjectionInverse", view_proj_inv);

        gfxCommandBindKernel(gfx, draw_sky_kernel_);
        gfxCommandDraw(gfx, 3);
    }
}


TRef<Texture> TextureLoader::LoadFromFile(std::string name, std::filesystem::path resource_path) {
    if (!resource_path.is_absolute()) {
        resource_path = GetInfra().GetResourceDirectory() / resource_path;
    }
    if (!std::filesystem::exists(resource_path)) {
        MI_WARN("TextureLoader: Texture file {} does not exist.", resource_path.string());
        return nullptr;
    }
    if (resource_path.extension() != ".png" && resource_path.extension() != ".jpg" && resource_path.extension() != ".jpeg") {
        MI_WARN("TextureLoader: Unsupported image format {} for {}.", resource_path.extension().string(), resource_path.string());
        return nullptr;
    }
    // Read binary data
    std::ifstream file(resource_path, std::ios::binary);
    if (!file) {
        MI_WARN("TextureLoader: Failed to open texture file {}.", resource_path.string());
        return nullptr;
    }
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    file.read(reinterpret_cast<char *>(buffer.data()), size);
    if (!file) {
        MI_WARN("TextureLoader: Failed to read texture file {}.", resource_path.string());
        return nullptr;
    }
    file.close();
    // Load texture from buffer
    std::string ext_name = resource_path.extension().string();
    std::string mime = "image/png";
    if (ext_name == ".jpg" || ext_name == ".jpeg") {
        mime = "image/jpeg";
    }
    auto texture = LoadFromBuffer(resource_path.string(), mime, buffer.data(), size);
    return texture;
}


TRef<Texture> TextureLoader::LoadFromBuffer(const std::string& name, const std::string & mime_type, const void *ptr, size_t size) {

    // Check mime type
    if (mime_type != "image/png" && mime_type != "image/jpeg") {
        MI_WARN("TextureLoader: Unsupported image format {} for {}.", mime_type, name);
        return nullptr;
    }

    TRef<Texture> texture;
    int width, height, channels;
    unsigned char *data = stbi_load_from_memory(
        static_cast<const stbi_uc *>(ptr),
        static_cast<int>(size),
        &width,
        &height,
        &channels,
        0  // Reserve channel number
    );

    if (!data) {
        // Failure
        return nullptr;
    }

    PixelFormatType format;
    bool pad_alpha = false;
    switch (channels) {
        case 1: format = PixelFormatType::kR8_UNORM; break;
        case 2: format = PixelFormatType::kR8G8_UNORM; break;
        case 3: pad_alpha = true;
        case 4: format = PixelFormatType::kR8G8B8A8_UNORM; break;
        default:
            stbi_image_free(data);
            return nullptr;
    }
    auto init_data = data;
    if (pad_alpha) {
        init_data = new uint8_t[width * height * 4];
        for (int i = 0; i < width * height; ++i) {
            init_data[i * 4 + 0] = data[i * channels + 0];
            init_data[i * 4 + 1] = data[i * channels + 1];
            init_data[i * 4 + 2] = data[i * channels + 2];
            init_data[i * 4 + 3] = 255;
        }
        channels = 4;
    }

    texture = Texture::Create(format, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    texture->InitializeFromBinary(std::span<uint8_t>(init_data, width * height * channels));
    texture->SetName(name);

    stbi_image_free(data);
    if (init_data != data) {
        delete[] init_data;
    }

    return texture;
}

MI_NAMESPACE_END