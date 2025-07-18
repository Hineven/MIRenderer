/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_FWD_H
#define MI_RENDERER_FWD_H

#include <concepts>
#include <glm/glm.hpp>
#include "core/common.h"
MI_NAMESPACE_BEGIN

class Renderable;
class DeviceMaterial;
class Material;
struct Transform;
class Geometry;
class StaticMesh;
class StaticMeshInstance;
class Texture;

class DeviceScene;
class Scene;

class BatchedUploadContext;
struct RendererView;

class DeviceBindlessResourceAllocator;
class DeviceBufferHeapInterface;
class DeviceBufferHeapBuffer;

struct ViewCommonShaderParameters;

enum class MinimumMaterialFlagBits : unsigned {
    kNone = 0,
    kDoubleSided = 1 << 0
};

template<typename T>
concept CRenderable = std::derived_from<T, Renderable>;

MI_NAMESPACE_END
#endif //MI_RENDERER_FWD_H
