/*
 * Created: 2026/4/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_RAY_TRACING_REGISTRY_H
#define RDG_RAY_TRACING_REGISTRY_H

#include "core/common.h"
#include <string>

MI_NAMESPACE_BEGIN

// Info for a single ray-traced renderable class.
struct RayTracedRenderableClassInfo {
    const char* name;       // Human-readable name (e.g., "StaticMesh")
    const char* hlsl_name;  // Name used in HLSL entry points (e.g., "StaticMesh")
};

// Registry for ray-traced renderable classes.
// Defines the mapping between renderable class index and hitgroup info.
// This is used by RDG to construct per-renderable-class hitgroup entry point names and SBT layouts.
//
// Entry point naming convention:
//   Given a shader template prefix (e.g., "TraceShadowRays"), RDG constructs entry point names as:
//     {Template}ClosestHit_{ClassName}  and  {Template}AnyHit_{ClassName}
//   where {ClassName} comes from the registered class's hlsl_name.
//
// Example:
//   Template = "TraceShadowRays"
//   → TraceShadowRaysClosestHit_StaticMesh, TraceShadowRaysAnyHit_StaticMesh       (StaticMesh)
//   → TraceShadowRaysClosestHit_VolumeGrid, TraceShadowRaysAnyHit_VolumeGrid       (VolumeGrid)
//   → TraceShadowRaysClosestHit_VolumePrimitives, TraceShadowRaysAnyHit_VolumePrimitives  (VolumePrimitives)
//   → TraceShadowRaysClosestHit_GaussianRadianceField, TraceShadowRaysAnyHit_GaussianRadianceField  (GaussianRadianceField)
//
// If an entry point is not found in the compiled SPIR-V, RDG emits a warning and treats the
// corresponding hitgroup slot as a no-op (closest_hit=UINT32_MAX, any_hit=UINT32_MAX).
// Per Vulkan spec, triangle geometry with no hit shaders behaves as an opaque hit.
class RayTracedRenderableClassRegistry {
public:
    static constexpr uint32_t kMaxRayTracingHitGroups = 8;

    // Returns the number of registered ray-traced renderable classes.
    static uint32_t NumClasses();

    // Returns the class info for a given hitgroup index (0..NumClasses()-1).
    static const RayTracedRenderableClassInfo* GetClassInfo(uint32_t index);

    // Returns the HLSL name for a given hitgroup index.
    static const char* GetHLSLName(uint32_t index);

    // Constructs the full closest-hit entry point name from a template prefix and class index.
    static std::string MakeClosestHitEntryPoint(const std::string& template_prefix, uint32_t class_index);

    // Constructs the full any-hit entry point name from a template prefix and class index.
    static std::string MakeAnyHitEntryPoint(const std::string& template_prefix, uint32_t class_index);

    // Internal: called by RayTracedRenderableClassRegistrator to register a class.
    static uint32_t RegisterClass(const char* name, const char* hlsl_name);

private:
    static RayTracedRenderableClassInfo& GetClassInfoStorage(uint32_t index);
    static uint32_t& GetClassCountStorage();
};

// Registrator for a ray-traced renderable class.
// Usage: in a .cpp file, write:
//   static RayTracedRenderableClassRegistrator g_reg_static_mesh("StaticMesh", "StaticMesh");
// This registers the class at static initialization time.
class RayTracedRenderableClassRegistrator {
public:
    FORCEINLINE RayTracedRenderableClassRegistrator(const char* name, const char* hlsl_name) {
        RayTracedRenderableClassRegistry::RegisterClass(name, hlsl_name);
    }
};

MI_NAMESPACE_END
#endif //RDG_RAY_TRACING_REGISTRY_H
