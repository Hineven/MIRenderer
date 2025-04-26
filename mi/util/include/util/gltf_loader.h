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

class GLTFGeometryFactory : public Geometry {
public:
    friend class GLTFLoader;
};

class GLTFLoader {
public:
    static bool LoadGLTF (
        std::filesystem::path path, RenderResourceAllocator & allocator,
        World & world,
        std::vector<TRef<Geometry>> & geometries,
        std::vector<TRef<Material>> & materials,
        std::vector<TRef<StaticMesh>> & meshes
    );
    // Load SRV image
    static TRef<Texture> LoadImage (std::string name, std::filesystem::path path);
    static TRef<Texture> LoadImageFromBuffer (std::string name, const void * ptr, size_t size);
};

MI_NAMESPACE_END

#endif //GLTF_LOADER_H
