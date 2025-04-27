/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_FWD_H
#define MI_RENDERER_FWD_H

#include <glm/glm.hpp>
#include "core/common.h"
MI_NAMESPACE_BEGIN

class Renderable;
class DeviceMaterial;
class Material;
struct Transform;
class Geometry;
class StaticMesh;
class Texture;
class BindlessRendererTexture;

class World;

class RenderResourceAllocator;
class GPUBufferHeapInterface;
class GPUBufferHeapBuffer;

enum class MinimumMaterialFlagBits : unsigned {
    kNone = 0,
    kDoubleSided = 1 << 0
};

struct MinimumMaterial {
    glm::vec3 albedo_ {0.5f};
    float alpha_ {1.f};
    glm::vec3 emissive_ {0.f};
    float roughness_ {0.5f};
    glm::vec3 specular_ {0.f};
    uint32_t flags_ {};
    // Maps using UV0 (0xffffffffu for no map)
    // index the maps using the renderer readonly texture array.
    uint32_t albedo_map_ {UINT32_MAX};
    uint32_t normal_map_ {UINT32_MAX};
    uint32_t emissive_map_ {UINT32_MAX};
    uint32_t roughness_map_ {UINT32_MAX};
};

// Default vertex format
struct DefaultStaticMeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

MI_NAMESPACE_END
#endif //MI_RENDERER_FWD_H
