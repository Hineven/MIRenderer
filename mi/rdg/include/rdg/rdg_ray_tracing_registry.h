/*
 * Created: 2026/4/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_RAY_TRACING_REGISTRY_H
#define RDG_RAY_TRACING_REGISTRY_H

#include "core/common.h"
#include <string>
#include <vector>

MI_NAMESPACE_BEGIN

// Info for a single ray-traced renderable class.
struct RayTracedRenderableClassInfo {
    const char* name;       // Human-readable name (e.g., "StaticMesh")
    const char* hlsl_name;  // Name used in HLSL entry points (e.g., "StaticMesh")
};

// Registry for ray-traced renderable classes.
// Assigns class indices dynamically at startup via RegisterClass().
// Both C++ code and shaders consume the same runtime indices:
//   - C++: GetRayTracedClassIndex() returns the registered index
//   - Shaders: RDGShader injects MI_RENDERABLE_TYPE_XXXX macros at compile time
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

    // Query the registered index for a class by name.
    // Returns kMaxRayTracingHitGroups if not found (should never happen for registered classes).
    static uint32_t GetClassIndex(const char* name);

    // Constructs the full closest-hit entry point name from a template prefix and class index.
    static std::string MakeClosestHitEntryPoint(const std::string& template_prefix, uint32_t class_index);

    // Constructs the full any-hit entry point name from a template prefix and class index.
    static std::string MakeAnyHitEntryPoint(const std::string& template_prefix, uint32_t class_index);

    // Generates MI_RENDERABLE_TYPE_XXXX=value macros for shader injection.
    // Called by RDGShader::GetBaseDefaultMacros() to inject into all shaders.
    static std::vector<std::string> GetRenderableTypeMacros();

    // Internal: called by RayTracedRenderableClassRegistrator to register a class.
    // Returns the dynamically assigned index for this class.
    static uint32_t RegisterClass(const char* name, const char* hlsl_name);

private:
    static RayTracedRenderableClassInfo& GetClassInfoStorage(uint32_t index);
    static uint32_t& GetClassCountStorage();
};

// Registrator for a ray-traced renderable class.
// Wraps the key information for a renderable class (name, hlsl_name, class_index).
// Usage: declare as a static class member in the renderable header, define in the .cpp:
//   // Header:
//   static RayTracedRenderableClassRegistrator<MyRenderable> kClassRegistrator;
//   // .cpp:
//   RayTracedRenderableClassRegistrator<MyRenderable> MyRenderable::kClassRegistrator("MyRenderable", "MyRenderable");
template<typename T>
class RayTracedRenderableClassRegistrator {
public:
    FORCEINLINE RayTracedRenderableClassRegistrator(const char* name, const char* hlsl_name)
        : name_(name), hlsl_name_(hlsl_name),
          class_index_(RayTracedRenderableClassRegistry::RegisterClass(name, hlsl_name)) {}

    FORCEINLINE uint32_t GetClassIndex() const { return class_index_; }
    FORCEINLINE const char* GetName() const { return name_; }
    FORCEINLINE const char* GetHLSLName() const { return hlsl_name_; }

private:
    const char* name_;
    const char* hlsl_name_;
    uint32_t class_index_;
};

MI_NAMESPACE_END
#endif //RDG_RAY_TRACING_REGISTRY_H
