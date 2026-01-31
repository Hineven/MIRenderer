#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>
#include <glm/mat4x4.hpp>

#include <core/refcounted.h>
#include <core/util/slot_allocator.h>
#include <renderer/mi_transform.h>
#include <renderer/mi_renderable.h>

MI_NAMESPACE_BEGIN

class RenderableNode;

// Weak registry. Make sure all nodes it allocated are properly released before destroying the registry.
class RenderableNodeRegistry : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class RenderableNode;
    ~RenderableNodeRegistry() override;

    TRef<RenderableNode> Create(const std::string& name = "");

    RenderableNode * GetByIndex (uint32_t index) const;

    FORCEINLINE static TRef<RenderableNodeRegistry> Create() {
        return TRef<RenderableNodeRegistry>(new RenderableNodeRegistry());
    }
    FORCEINLINE uint32_t GetMaxNumRenderableNodes () const {
        return renderable_node_slots_.GetMaxNumSlots();
    }
protected:
    // id -> node registry
    std::map<uint32_t, RenderableNode*> renderable_node_registry_;
    ExtendableSlotAllocator renderable_node_slots_;

    uint32_t AllocateRenderableNodeIndex ();
    void FreeRenderableNodeIndex (uint32_t index);
};

// Lightweight scene-graph node sitting above renderer::Renderable.
class RenderableNode : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class RenderableNodeRegistry;
    virtual ~RenderableNode() override;

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
    explicit RenderableNode(RenderableNodeRegistry * registry, const std::string& name, uint32_t id);
    static Transform Compose(const Transform& parent, const Transform& local);
    static glm::mat4 ToMat4(const Transform& t);

    std::string name_;
    RenderableNodeRegistry * registry_ {nullptr};
    uint32_t index_ {0};

    Transform local_transform_{};
    Transform world_transform_{};
    RenderableNode* parent_ {nullptr};
    std::vector<TRef<RenderableNode>> children_;
    TRef<Renderable> renderable_;
};

MI_NAMESPACE_END

