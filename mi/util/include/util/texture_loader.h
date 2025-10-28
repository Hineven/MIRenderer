/*
 * Created: 2025/5/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef TEXTURE_LOADER_H
#define TEXTURE_LOADER_H

#include <filesystem>
#include <renderer/mi_geometry.h>

MI_NAMESPACE_BEGIN

class TextureLoader {
public:
    // Load an environment map and convert to cubic texture. This function dispatches shaders individually.
    static TRef<Texture> LoadEnvironmentMap (std::string name, std::filesystem::path resource_path);
    // Load an environment map and convert to cubic texture. This function dispatches shaders individually.
    static TRef<Texture> LoadEnvironmentMapFromBuffer(const std::string& name, const std::string & mime_type, const void *ptr, size_t size) ;
    static TRef<Texture> LoadFromFile (std::string name, std::filesystem::path resource_path);
    // Load a texture from a binary with a specific MIME type.
    static TRef<Texture> LoadFromBuffer(const std::string& name, const std::string & mime_type, const void *ptr, size_t size) ;

    // Helper function. Runs on CPU
    static bool IsTextureOpaque(Texture * texture);
};

MI_NAMESPACE_END


#endif //TEXTURE_LOADER_H
