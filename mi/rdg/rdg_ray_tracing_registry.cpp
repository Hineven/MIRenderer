/*
 * Created: 2026/4/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rdg/rdg_ray_tracing_registry.h"
#include <cassert>
#include <cstring>

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

uint32_t RayTracedRenderableClassRegistry::GetClassIndex(const char* name) {
    for (uint32_t i = 0; i < g_class_count; ++i) {
        if (strcmp(g_class_info_storage[i].name, name) == 0) {
            return i;
        }
    }
    return kMaxRayTracingHitGroups; // Not found
}

std::vector<std::string> RayTracedRenderableClassRegistry::GetRenderableTypeMacros() {
    std::vector<std::string> macros;
    macros.reserve(g_class_count);
    for (uint32_t i = 0; i < g_class_count; ++i) {
        auto info = GetClassInfo(i);
        if (info) {
            std::string macro = "MI_RENDERABLE_TYPE_";
            macro += info->name;
            macro += "=" + std::to_string(i);
            macros.emplace_back(std::move(macro));
        }
    }
    return macros;
}

std::string RayTracedRenderableClassRegistry::MakeClosestHitEntryPoint(const std::string& template_prefix, uint32_t class_index) {
    return template_prefix + "ClosestHit_" + GetHLSLName(class_index);
}

std::string RayTracedRenderableClassRegistry::MakeAnyHitEntryPoint(const std::string& template_prefix, uint32_t class_index) {
    return template_prefix + "AnyHit_" + GetHLSLName(class_index);
}

uint32_t RayTracedRenderableClassRegistry::RegisterClass(const char* name, const char* hlsl_name) {
    // Check if already registered
    for (uint32_t i = 0; i < g_class_count; ++i) {
        if (strcmp(g_class_info_storage[i].name, name) == 0) {
            return i; // Return existing index
        }
    }
    assert(g_class_count < kMaxRayTracingHitGroups);
    uint32_t index = g_class_count;
    g_class_info_storage[index] = {name, hlsl_name};
    g_class_count = index + 1;
    return index;
}

RayTracedRenderableClassInfo& RayTracedRenderableClassRegistry::GetClassInfoStorage(uint32_t index) {
    return g_class_info_storage[index];
}

uint32_t& RayTracedRenderableClassRegistry::GetClassCountStorage() {
    return g_class_count;
}

MI_NAMESPACE_END
