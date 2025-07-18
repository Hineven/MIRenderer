/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef GLTF_LOADER_H
#define GLTF_LOADER_H

#include <filesystem>
#include <renderer/mi_geometry.h>

MI_NAMESPACE_BEGIN

class GLTFLoader {
public:
    static bool LoadGLTF (
        std::filesystem::path path, DeviceBindlessResourceAllocator & allocator,
        Scene & scene,
        std::vector<TRef<Geometry>> & out_geometries,
        std::vector<TRef<Material>> & out_materials,
        std::vector<TRef<StaticMeshInstance>> & out_meshes
    );
    // Load SRV image
    // static TRef<Texture> LoadImage (std::string name, std::filesystem::path path);
    // static TRef<Texture> LoadImageFromBuffer (const std::string& name, const std::string & mime_type, const void * ptr, size_t size);
};

MI_NAMESPACE_END

#endif //GLTF_LOADER_H
