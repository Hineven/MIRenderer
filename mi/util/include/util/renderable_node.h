#pragma once

#include <string>
#include <vector>
#include <optional>
#include <glm/mat4x4.hpp>
#include <renderer/mi_transform.h>
#include <renderer/mi_renderable.h>
#include <core/refcounted.h>

MI_NAMESPACE_BEGIN

// Lightweight scene-graph node sitting above renderer::Renderable.
class RenderableNode : public RefCounted<> {
public:
    static TRef<RenderableNode> Create(const std::string& name = "");

    void SetName(const std::string& name) { name_ = name; }
    const std::string& GetName() const { return name_; }

    void SetRenderable(Renderable* r);
    Renderable* GetRenderable() const { return renderable_.Raw(); }

    void SetLocalTransform(const Transform& t);
    const Transform& GetLocalTransform() const { return local_transform_; }

    // Call after changing local or parent to refresh world transform down the tree.
    void UpdateWorldTransform(const Transform* parent_world = nullptr);
    const Transform& GetWorldTransform() const { return world_transform_; }

    // Parent is non-owning to avoid cycles.
    RenderableNode* GetParent() const { return parent_; }
    void AddChild(TRef<RenderableNode> child);
    const std::vector<TRef<RenderableNode>>& GetChildren() const { return children_; }

private:
    explicit RenderableNode(const std::string& name);
    static Transform Compose(const Transform& parent, const Transform& local);
    static glm::mat4 ToMat4(const Transform& t);

    std::string name_;
    Transform local_transform_{};
    Transform world_transform_{};
    RenderableNode* parent_ {nullptr};
    std::vector<TRef<RenderableNode>> children_;
    TRef<Renderable> renderable_;
};

MI_NAMESPACE_END

