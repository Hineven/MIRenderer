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

struct GeometryBufferData;
struct WorldRadianceCacheData;
struct LightStructureData;
struct DiffuseIndirectLightingData;
struct DenoiserViewData;
struct VolumeGridDirectLightingData;
struct DiffuseDirectLightingData;
// struct VolumeGridIndirectLightingData;
struct DebugCommonShaderParameters;

struct GeometryBufferPersistentData;
struct DiffuseIndirectLightingPersistentData;
struct DenoiserPersistentData;
struct LightStructurePersistentData;
struct HashGridPersistentData;
struct DebugPersistentData;

struct RendererViewPersistentData;
struct RendererView;

class DeviceBindlessResourceAllocator;
class DeviceBindlessResourceSlotKeeper;
class DeviceBufferHeapInterface;
class DeviceBufferHeapBuffer;
class DeviceUberBufferAllocation;
class DeviceUberBufferInterface;
class DeviceUberBufferArrayAllocation;
class DeviceUberBufferArrayInterface;

struct ViewCommonShaderParameters;

template<typename T>
concept CRenderable = std::derived_from<T, Renderable>;

enum class VisibilityTraceType {
    kCoarse = 0,
    kCoarseWithExactVolumeScattering = 1,
    kFull = 2
};

class DeviceGigaVoxel;
class GigaVoxel;
class GigaVoxelInstance;

class NGXContext;
class DLSSRRContext;

MI_NAMESPACE_END
#endif //MI_RENDERER_FWD_H
