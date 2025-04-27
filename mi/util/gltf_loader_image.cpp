/*
 * Created: 2025/4/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "util/gltf_loader.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "core/infra.h"
#include "renderer/mi_texture.h"

MI_NAMESPACE_BEGIN
TRef<Texture> GLTFLoader::LoadImageFromBuffer(const std::string& name, const std::string & mime_type, const void *ptr, size_t size) {

    // Check mime type
    if (mime_type != "image/png" && mime_type != "image/jpeg") {
        MI_WARN("GLTFLoader: Unsupported image format {} for {}.", mime_type, name);
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