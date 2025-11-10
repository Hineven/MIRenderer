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

struct VolumePrimitivesViewData;
struct WorldRadianceCacheData;
struct LightStructureData;
struct DiffuseIndirectLightingData;
struct DenoiserViewData;
struct VolumeDirectLightingData;
struct DiffuseDirectLightingData;
struct VolumeIndirectLightingData;
struct DebugCommonShaderParameters;

struct DiffuseIndirectLightingPersistentData;
struct DenoiserPersistentData;
struct LightStructurePersistentData;
struct HashGridPersistentData;
struct VolumePrimitivesViewPersistentData;

struct RendererViewPersistentData;
struct RendererView;

class DeviceBindlessResourceAllocator;
class DeviceBufferHeapInterface;
class DeviceBufferHeapBuffer;
class DeviceUberBufferAllocation;
class DeviceUberBufferInterface;

struct ViewCommonShaderParameters;

template<typename T>
concept CRenderable = std::derived_from<T, Renderable>;

MI_NAMESPACE_END
#endif //MI_RENDERER_FWD_H
