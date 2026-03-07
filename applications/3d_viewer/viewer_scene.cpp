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
    loaded_scenes_.push_back(std::move(s));
}

void ViewerApp::WaitForSceneMutation() {
    RHI::Get().WaitForIdle();
}

void ViewerApp::FlushSceneDelayedDestruction() {
    if (scene_) {
        scene_->ForceFlushDelayedDestruction();
    }
    if (resource_allocator_) {
        resource_allocator_->ForceFlushDelayedDestruction();
    }
}

void ViewerApp::UnloadScene(size_t idx) {
    if (idx >= loaded_scenes_.size()) return;

    WaitForSceneMutation();
    FlushSceneDelayedDestruction();

    auto& s = loaded_scenes_[idx];
    for (auto& root : s.roots) {
        std::function<void(TRef<RenderableNode>)> walk = [&](TRef<RenderableNode> n) {
            if (!n) return;
            if (auto r = n->GetRenderable()) {
                if (selection_state_.selected_renderable_index == r->GetIndex()) {
                    SetSelectedRenderable(nullptr);
                }
            }
            for (auto& c : n->GetChildren()) walk(c);
        };
        walk(root);
    }
    s.roots.clear();
    loaded_scenes_.erase(loaded_scenes_.begin() + static_cast<std::ptrdiff_t>(idx));

    FlushSceneDelayedDestruction();
}

MI_NAMESPACE_END

