/*
 * Created: 2026/4/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <renderer/mi_renderer_export.h>

MI_NAMESPACE_BEGIN

RendererExports::RendererExports() = default;

RendererExports::~RendererExports() = default;

RDGResource * RendererExports::Get(const std::string & export_id) const {
    auto it = exportable_resources_.find(export_id);
    if (it == exportable_resources_.end()) return nullptr;
    return it->second.Raw();
}

bool RendererExports::RequestExport(const std::string & export_id) const {
    auto it = exportable_resources_.find(export_id);
    if (it == exportable_resources_.end() || !it->second) return false;
    it->second->SetExport();
    return true;
}

void RendererExports::RegisterResource(const std::string & export_id, RDGResource * resource) {
    exportable_resources_[export_id] = resource;
}

MI_NAMESPACE_END
