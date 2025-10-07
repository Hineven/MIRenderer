/*
 * Created: 2025/5/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <fstream>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <util/texture_loader.h>
#include "core/infra.h"
#include "rdg/rdg.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "rdg/rdg_shader.h"
#include "renderer/mi_texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"

#define M_PI 3.14159265358979323846

MI_NAMESPACE_BEGIN

class MappingShader : public RDGShader {
public:
    DECLARE_SHADER()
    struct MappingShaderUB {
        glm::mat4 ViewProjectionInverse;
        uint2 TextureDimensions;
        uint2 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(MappingShaderUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, InEnvironmentMap)
        SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, OutEnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, InSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_GRAPHICS_SHADER(MappingShader, "mi/util/shaders/texture_loader/MappingShader.hlsl", "VS_Main", "PS_Main")

TRef<Texture> TextureLoader::LoadEnvironmentMapFromBuffer(const std::string &name, const std::string &mime_type, const void *ptr, size_t size) {
    auto env_texture = LoadFromBuffer(name + "_env", mime_type, ptr, size);
    env_texture->UpdateOnDevice();

    glm::dvec3 const forward_vectors[] = {glm::dvec3(-1.0, 0.0, 0.0), glm::dvec3(1.0, 0.0, 0.0),
    glm::dvec3(0.0, 1.0, 0.0), glm::dvec3(0.0, -1.0, 0.0), glm::dvec3(0.0, 0.0, -1.0),
    glm::dvec3(0.0, 0.0, 1.0)};

    glm::dvec3 const up_vectors[] = {glm::dvec3(0.0, -1.0, 0.0), glm::dvec3(0.0, -1.0, 0.0),
        glm::dvec3(0.0, 0.0, -1.0), glm::dvec3(0.0, 0.0, 1.0), glm::dvec3(0.0, -1.0, 0.0),
        glm::dvec3(0.0, -1.0, 0.0)};

    // 1k res
    constexpr auto face_resolution = 1024;
    auto env_cubemap = Texture::Create(RHITextureType::kCube, PixelFormatType::kR8G8B8A8_UNORM, face_resolution, face_resolution, 6);
    env_cubemap->AddDeviceUsage(RHITextureUsageFlagBits::kRenderTarget);
    env_cubemap->UpdateOnDevice();
    // Make sure pool is destroyed after the render graph
    auto pool = RDGResourcePool::Create();
    {
        auto mapping_shader = RDGShaderLibrary::Get().GetShader<MappingShader>();
        MappingShader::ShaderParameters template_params;
        RenderGraphBuilder builder;
        env_cubemap->UpdateOnDevice();
        template_params.InEnvironmentMap  = builder.Import(env_texture->GetDeviceTexture());
        template_params.OutEnvironmentMap = builder.Import(env_cubemap->GetDeviceTexture());
        template_params.OutEnvironmentMap.load_op = RHILoadOpType::kClear;
        template_params.InSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

        for (uint32_t cubemap_face = 0; cubemap_face < 6; ++cubemap_face)
        {
            auto params = builder.Allocate<MappingShader::ShaderParameters>();
            *params = template_params;
            params->OutEnvironmentMap.array_layer = cubemap_face;
            glm::dmat4 const view =
                glm::lookAt(glm::dvec3(0.0), forward_vectors[cubemap_face], up_vectors[cubemap_face]);
            glm::dmat4 const proj          = glm::perspective(M_PI / 2.0, 1.0, 0.1, 1e4);
            glm::mat4 const  view_proj_inv = glm::mat4(glm::inverse(proj * view));
            params->UB = builder.Allocate<MappingShader::MappingShaderUB>();
            params->UB->TextureDimensions = {face_resolution, face_resolution};
            params->UB->ViewProjectionInverse = view_proj_inv;
            builder.AddPass<MappingShader>(RDGPassFlagBits::kNeverCull, mapping_shader, params, [
                params, mapping_shader
            ](RDGPass * pass, RHICommandQueueGraphics & queue) {
                RDGCommandHelper::Draw<MappingShader>(queue, pass, mapping_shader, params, 3);
            });
        }
        builder.Compile()->Execute(pool.Raw());
        // manual barrier
        RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
            env_cubemap->GetDeviceTexture(),
            RHITextureLayoutType::kShaderReadOnlyOptimal,
            RHIPipelineStageFlagBits::kAll,
            RHIPipelineStageFlagBits::kAll,
            RHIGPUAccessFlagBits::kRW,
            RHIGPUAccessFlagBits::kRW
        );
        RHI::Get().GetGraphicsCommandQueue().WaitForIdle("TextureLoader::LoadEnvironmentMapFromBuffer " + name);
    }
    return env_cubemap;
}

TRef<Texture> TextureLoader::LoadEnvironmentMap(std::string name, std::filesystem::path resource_path) {
    if (!resource_path.is_absolute()) {
        resource_path = GetInfra().GetResourceDirectory() / resource_path;
    }
    if (!std::filesystem::exists(resource_path)) {
        MI_WARN("TextureLoader: Texture file {} does not exist.", resource_path.string());
        return nullptr;
    }
    if (resource_path.extension() != ".png" && resource_path.extension() != ".jpg"
        && resource_path.extension() != ".jpeg" && resource_path.extension() != ".exr") {
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
    if (ext_name == ".exr") {
        mime = "image/exr";
    }
    auto texture = LoadEnvironmentMapFromBuffer(resource_path.string(), mime, buffer.data(), size);
    return texture;
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
    if (mime_type != "image/png" && mime_type != "image/jpeg" && mime_type != "image/exr") {
        MI_WARN("TextureLoader: Unsupported image format {} for {}.", mime_type, name);
        return nullptr;
    }

    TRef<Texture> texture;
    int width, height, channels;
    if (mime_type == "image/exr") {
        float * rgba {};
        const char * err {};
        LoadEXRFromMemory(&rgba, &width, &height, static_cast<const unsigned char *>(ptr), size, &err);
        if (err) {
            MI_WARN("TextureLoader: Failed to load EXR image {}: {}", name, err);
            FreeEXRErrorMessage(err);
            return nullptr;
        } else {
            // Loaded EXR image is always 4 channeled
            auto * rgba_fp16 = static_cast<uint32_t *>(malloc(width * height * 4 * sizeof(uint16_t)));
            for (int i = 0; i < width * height * 4; i += 2) {
                rgba_fp16[i / 2] = glm::packHalf2x16({rgba[i], rgba[i + 1]});
            }
            free(rgba);
            texture = Texture::Create(PixelFormatType::kR16G16B16A16_FLOAT, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            texture->InitializeFromBinary(std::span<uint8_t>((uint8_t*)rgba_fp16, width * height * 4 * sizeof(uint16_t)));
            texture->SetName(name);
            free(rgba_fp16);
        }
    } else {
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
            MI_WARN("TextureLoader: Failed to load image {}: {}", name, stbi_failure_reason());
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
    }

    return texture;
}
MI_NAMESPACE_END