#define GLM_ENABLE_EXPERIMENTAL
#include "util/renderable_node.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <renderer/mi_scene.h>

MI_NAMESPACE_BEGIN

uint32_t RenderableNodeRegistry::AllocateRenderableNodeIndex() {
    return renderable_node_slots_.AllocateSlot();
}

void RenderableNodeRegistry::FreeRenderableNodeIndex(uint32_t index) {
    renderable_node_slots_.FreeSlot(index);
    renderable_node_registry_.erase(index);
}

RenderableNodeRegistry::~RenderableNodeRegistry() {
    mi_check_nothrow(renderable_node_slots_.NoAllocationActive(),
        "RenderableNodeRegistry destroyed with unreleased RenderableNodes.");
}

TRef<RenderableNode> RenderableNodeRegistry::Create(const std::string& name) {
    auto id = AllocateRenderableNodeIndex();
    auto node = TRef<RenderableNode>(new RenderableNode(this, name, id));
    renderable_node_registry_[id] = node.Raw();
    return node;
}

RenderableNode * RenderableNodeRegistry::GetByIndex(uint32_t index) const {
    auto it = renderable_node_registry_.find(index);
    if (it != renderable_node_registry_.end()) {
        return it->second;
    }
    return nullptr;
}

RenderableNode::~RenderableNode() {
    // Free index from registry
    registry_->FreeRenderableNodeIndex(index_);
}

RenderableNode::RenderableNode(RenderableNodeRegistry * reg, const std::string& name, uint32_t id) : name_(name), registry_(reg), index_(id) {
    world_transform_ = local_transform_;
}

void RenderableNode::SetRenderable(Renderable* r) {
    renderable_ = r;
    if (renderable_) {
        renderable_->EditTransform() = world_transform_;
    }
}

void RenderableNode::SetLocalTransform(const Transform& t) {
    local_transform_ = t;
    UpdateWorldTransform(parent_ ? &parent_->world_transform_ : nullptr);
}

void RenderableNode::TranslateLocal(const glm::vec3& delta) {
    local_transform_.position += delta;
    UpdateWorldTransform(parent_ ? &parent_->world_transform_ : nullptr);
}

void RenderableNode::RotateLocal(const glm::vec3& euler_angles_rad) {
    local_transform_.rotation += euler_angles_rad;
    UpdateWorldTransform(parent_ ? &parent_->world_transform_ : nullptr);
}

void RenderableNode::ScaleLocal(const glm::vec3& euler_angles_rad) {
    local_transform_.rotation += euler_angles_rad;
    UpdateWorldTransform(parent_ ? &parent_->world_transform_ : nullptr);
}


void RenderableNode::UpdateWorldTransform(const Transform* parent_world) {
    if (parent_world) {
        world_transform_ = Compose(*parent_world, local_transform_);
    } else {
        world_transform_ = local_transform_;
    }
    if (renderable_) {
        renderable_->SetTransform(world_transform_);
    }
    for (auto& child : children_) {
        if (child) child->UpdateWorldTransform(&world_transform_);
    }
}

void RenderableNode::AddChild(TRef<RenderableNode> child) {
    if (!child) return;
    child->parent_ = this;
    children_.push_back(child);
    child->UpdateWorldTransform(&world_transform_);
}

Transform RenderableNode::Compose(const Transform& parent, const Transform& local) {
    glm::mat4 parent_m = ToMat4(parent);
    glm::mat4 local_m = ToMat4(local);
    glm::mat4 world_m = parent_m * local_m;
    return Transform::FromMatrix(world_m);
}

glm::mat4 RenderableNode::ToMat4(const Transform& t) {
    glm::mat4 scale_m = glm::scale(glm::mat4(1.0f), t.scale);
    glm::quat q(t.rotation);
    glm::mat4 rot_m = glm::mat4_cast(q);
    glm::mat4 trans_m = glm::translate(glm::mat4(1.0f), t.position);
    return trans_m * rot_m * scale_m;
}

MI_NAMESPACE_END

