#include "viewer_app.h"
#include "util/renderable_node.h"

MI_NAMESPACE_BEGIN

void ViewerApp::SetSelectedRenderable(Renderable* renderable) {
    if (!renderable) {
        selection_state_.selected_renderable_index = UINT32_MAX;
        selection_state_.selected_deferred_renderable_index = UINT32_MAX;
        if (arrow_mesh_x_instance_) arrow_mesh_x_instance_->SetVisible(false);
        if (arrow_mesh_y_instance_) arrow_mesh_y_instance_->SetVisible(false);
        if (arrow_mesh_z_instance_) arrow_mesh_z_instance_->SetVisible(false);
        return;
    }
    selection_state_.selected_renderable_index = renderable->GetIndex();
    selection_state_.selected_deferred_renderable_index = renderable->GetIndex();
    auto pos = renderable->GetTransform().position;
    if (arrow_mesh_x_instance_) {
        arrow_mesh_x_instance_->EditTransform().position = pos;
        arrow_mesh_x_instance_->SetVisible(true);
    }
    if (arrow_mesh_y_instance_) {
        arrow_mesh_y_instance_->EditTransform().position = pos;
        arrow_mesh_y_instance_->SetVisible(true);
    }
    if (arrow_mesh_z_instance_) {
        arrow_mesh_z_instance_->EditTransform().position = pos;
        arrow_mesh_z_instance_->SetVisible(true);
    }
}

void ViewerApp::RegisterLoadedScene(const std::string& name, const std::vector<TRef<RenderableNode>>& roots) {
    LoadedScene s{name, roots};
    for (auto& root : roots) {
        std::function<void(TRef<RenderableNode>)> walk = [&](TRef<RenderableNode> n) {
            if (!n) return;
            if (auto r = n->GetRenderable()) {
                renderable_node_lookup_[r] = n;
            }
            for (auto& c : n->GetChildren()) walk(c);
        };
        walk(root);
    }
    loaded_scenes_.push_back(std::move(s));
}

void ViewerApp::UnloadScene(size_t idx) {
    if (idx >= loaded_scenes_.size()) return;
    auto& s = loaded_scenes_[idx];
    for (auto& root : s.roots) {
        std::function<void(TRef<RenderableNode>)> walk = [&](TRef<RenderableNode> n) {
            if (!n) return;
            if (auto r = n->GetRenderable()) {
                scene_->RemoveRenderable(r);
                renderable_node_lookup_.erase(r);
                if (selection_state_.selected_renderable_index == r->GetIndex()) {
                    SetSelectedRenderable(nullptr);
                }
            }
            for (auto& c : n->GetChildren()) walk(c);
        };
        walk(root);
    }
    s.roots.clear();
}

MI_NAMESPACE_END

