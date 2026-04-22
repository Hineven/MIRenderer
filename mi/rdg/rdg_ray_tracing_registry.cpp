/*
 * Created: 2026/4/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rdg/rdg_ray_tracing_registry.h"
#include <cassert>

MI_NAMESPACE_BEGIN

static RayTracedRenderableClassInfo g_class_info_storage[RayTracedRenderableClassRegistry::kMaxRayTracingHitGroups] {};
static uint32_t g_class_count = 0;

uint32_t RayTracedRenderableClassRegistry::NumClasses() {
    return g_class_count;
}

const RayTracedRenderableClassInfo* RayTracedRenderableClassRegistry::GetClassInfo(uint32_t index) {
    if (index >= g_class_count) return nullptr;
    return &g_class_info_storage[index];
}

const char* RayTracedRenderableClassRegistry::GetHLSLName(uint32_t index) {
    auto info = GetClassInfo(index);
    return info ? info->hlsl_name : "";
}

std::string RayTracedRenderableClassRegistry::MakeClosestHitEntryPoint(const std::string& template_prefix, uint32_t class_index) {
    return template_prefix + "ClosestHit_" + GetHLSLName(class_index);
}

std::string RayTracedRenderableClassRegistry::MakeAnyHitEntryPoint(const std::string& template_prefix, uint32_t class_index) {
    return template_prefix + "AnyHit_" + GetHLSLName(class_index);
}

uint32_t RayTracedRenderableClassRegistry::RegisterClass(const char* name, const char* hlsl_name) {
    assert(g_class_count < kMaxRayTracingHitGroups);
    uint32_t index = g_class_count++;
    g_class_info_storage[index] = {name, hlsl_name};
    return index;
}

RayTracedRenderableClassInfo& RayTracedRenderableClassRegistry::GetClassInfoStorage(uint32_t index) {
    return g_class_info_storage[index];
}

uint32_t& RayTracedRenderableClassRegistry::GetClassCountStorage() {
    return g_class_count;
}

MI_NAMESPACE_END
