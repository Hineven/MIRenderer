#include "viewer_control.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <glm/geometric.hpp>
#include "renderer/mi_scene.h"

MI_NAMESPACE_BEGIN

namespace {

static void DrawRenderableTree(ViewerApp& app, RenderableNode* node) {
    if (!node) return;
    auto* renderable = node->GetRenderable();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (renderable && app.selection_state_.selected_deferred_renderable_index == renderable->GetIndex()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    if (node->GetChildren().empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    std::string label = node->GetName().empty() ? "(unnamed)" : node->GetName();
    if (renderable) {
        label += std::format(" [{}]", renderable->GetIndex());
    }
    bool open = ImGui::TreeNodeEx(node, flags, "%s", label.c_str());
    if (ImGui::IsItemClicked()) {
        if (renderable) app.SetSelectedRenderable(renderable);
    }
    if (renderable) {
        bool visible = renderable->IsVisible();
        ImGui::SameLine();
        if (ImGui::Checkbox(std::format("##vis{}", (void*)renderable).c_str(), &visible)) {
            renderable->SetVisible(visible);
        }
    }
    if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        for (auto& c : node->GetChildren()) {
            DrawRenderableTree(app, c.Raw());
        }
        ImGui::TreePop();
    }
}

} // namespace

void ViewerControlUI::DrawControlUI(ViewerApp& app, ViewerApp::FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration) {
    auto& io = ImGui::GetIO();
    CVar_DebugCursorScreenCoordsX.Set((int)round(io.MousePos.x));
    CVar_DebugCursorScreenCoordsY.Set((int)round(io.MousePos.y));

    ImGui::TextUnformatted("Rendering");
    ImGui::Separator();

    if (ImGui::TreeNode("Scene Graph")) {
        for (size_t i = 0; i < app.loaded_scenes_.size(); ++i) {
            auto& s = app.loaded_scenes_[i];
            std::string lbl = std::format("{}##scene{}", s.name, i);
            if (ImGui::TreeNode(lbl.c_str())) {
                for (auto& root : s.roots) {
                    DrawRenderableTree(app, root.Raw());
                }
                if (ImGui::Button(std::format("Unload##{}", i).c_str())) {
                    app.UnloadScene(i);
                }
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }

    if (ImGui::CollapsingHeader("Direct Lighting")) {
        auto& directional_light = app.scene_->directional_light_;
        ImGui::Checkbox("Enabled", &directional_light.enabled);
        if (ImGui::DragFloat3("Direction", &directional_light.direction[0], 0.01f)) {
            if (glm::length(directional_light.direction) > 1e-6f) {
                directional_light.direction = glm::normalize(directional_light.direction);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Normalize")) {
            if (glm::length(directional_light.direction) > 1e-6f) {
                directional_light.direction = glm::normalize(directional_light.direction);
            }
        }
        ImGui::ColorEdit3("Color", &directional_light.color[0]);
        if (ImGui::DragFloat("Intensity", &directional_light.intensity, 0.05f, 0.0f, 1000.0f)) {
            directional_light.intensity = std::max(0.0f, directional_light.intensity);
        }
    }

    // Existing UI moved from ViewerApp::HandleControlUILogic
    if (ImGui::TreeNode("Pinned CVars")) {
        auto DrawImGuiControlForCVar = [&](CVarBase& e) {
            auto cvar_name = e.GetId();
            if (e.GetType() == CVarType::kBool) {
                auto cvar = static_cast<CVar<bool>*>(&e);
                bool value = cvar->Get();
                if (ImGui::Checkbox(cvar_name.c_str(), &value)) {
                    cvar->Set(value);
                }
            } else if (e.GetType() == CVarType::kFloat) {
                auto cvar = static_cast<CVar<float>*>(&e);
                float value = cvar->Get();
                if (ImGui::DragFloat(cvar_name.c_str(), &value, 0.01f)) {
                    cvar->Set(value);
                }
            } else if (e.GetType() == CVarType::kFloat2) {
                auto cvar = static_cast<CVar<glm::vec2>*>(&e);
                glm::vec2 value = cvar->Get();
                if (ImGui::DragFloat2(cvar_name.c_str(), &value[0], 0.01f)) {
                    cvar->Set(value);
                }
            } else if (e.GetType() == CVarType::kFloat3) {
                auto cvar = static_cast<CVar<glm::vec3>*>(&e);
                glm::vec3 value = cvar->Get();
                if (ImGui::DragFloat3(cvar_name.c_str(), &value[0], 0.01f)) {
                    cvar->Set(value);
                }
            } else if (e.GetType() == CVarType::kFloat4) {
                auto cvar = static_cast<CVar<glm::vec4>*>(&e);
                glm::vec4 value = cvar->Get();
                if (ImGui::DragFloat4(cvar_name.c_str(), &value[0], 0.01f)) {
                    cvar->Set(value);
                }
            } else if (e.GetType() == CVarType::kInt) {
                auto cvar = static_cast<CVar<int>*>(&e);
                int value = cvar->Get();
                if (ImGui::DragInt(cvar_name.c_str(), &value)) {
                    cvar->Set(value);
                }
            } else if (e.GetType() == CVarType::kString) {
                auto cvar = static_cast<CVar<std::string>*>(&e);
                std::string value = cvar->Get();
                char buffer[256];
                strncpy_s(buffer, value.c_str(), sizeof(buffer));
                if (ImGui::InputText(cvar_name.c_str(), buffer, sizeof(buffer))) {
                    cvar->Set(std::string(buffer));
                }
            }
        };
        ImGui::Indent(20);
        for (auto & cvar : app.pinned_cvars_) {
            DrawImGuiControlForCVar(*cvar);
        }
        ImGui::Unindent(20);
        ImGui::TreePop();
    }
    if (ImGui::Button("Reload Shaders")) {
        ops.should_reload_shaders = true;
    }

    if (ImGui::CollapsingHeader("Persistent Cameras")) {
        // Save current view as a new persistent camera.
        // Local ImGui input state (persists across frames for this panel).
        static char name_buf[128] = "view";
        static int new_priority = 0;
        ImGui::TextDisabled("Save current view as a persistent camera:");
        ImGui::PushItemWidth(-1);
        ImGui::InputText("##new_pcam_name", name_buf, sizeof(name_buf));
        ImGui::PopItemWidth();
        ImGui::InputInt("Priority##new_pcam", &new_priority);
        ImGui::SameLine();
        if (ImGui::Button("Save##new_pcam")) {
            app.SaveCurrentCameraAsPersistent(std::string(name_buf), new_priority);
        }

        ImGui::Separator();

        if (app.persistent_cameras_.empty()) {
            ImGui::TextDisabled("(no persistent cameras saved)");
        } else {
            // Build a priority-descending view order so the list always reads
            // highest-priority-first, regardless of storage order.
            std::vector<size_t> order(app.persistent_cameras_.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                if (app.persistent_cameras_[a].priority != app.persistent_cameras_[b].priority)
                    return app.persistent_cameras_[a].priority > app.persistent_cameras_[b].priority;
                return a < b;
            });

            // Compact single-line rows: priority is an inline editable InputInt
            // (auto-saves on change), and only Apply / Upd / Del buttons remain.
            // We need a mutable accessor since the list is reordered for display.
            auto get_camera_ref = [&](size_t idx) -> PersistentCamera& {
                return app.persistent_cameras_[idx];
            };

            for (size_t row = 0; row < order.size(); ++row) {
                const size_t idx = order[row];
                ImGui::PushID(static_cast<int>(idx));

                // Priority column (editable, persists on change).
                ImGui::SetNextItemWidth(48.0f);
                int prio = get_camera_ref(idx).priority;
                if (ImGui::InputInt("##pcam_prio", &prio, 0)) {
                    app.MovePersistentCameraPriority(idx, prio - get_camera_ref(idx).priority);
                }
                ImGui::SameLine(0.0f, 6.0f);

                // Name column (truncated display only).
                ImGui::AlignTextToFramePadding();
                const std::string& nm = get_camera_ref(idx).name;
                ImGui::TextUnformatted(nm.c_str());
                ImGui::SameLine(0.0f, 6.0f);

                // Action buttons, right-aligned to fill remaining width.
                const float btn_w = 46.0f;
                const float spacing = ImGui::GetStyle().ItemSpacing.x;
                const float used = 48.0f + 6.0f + ImGui::CalcTextSize(nm.c_str()).x + 6.0f;
                const float avail = ImGui::GetContentRegionAvail().x;
                const float buttons_total = btn_w * 3 + spacing * 2;
                // Push buttons to the right edge if there is room.
                if (avail > used + buttons_total) {
                    ImGui::Dummy(ImVec2(avail - used - buttons_total, 0));
                    ImGui::SameLine(0.0f, 0.0f);
                }

                if (ImGui::Button("Apply", ImVec2(btn_w, 0))) {
                    app.ApplyPersistentCamera(idx);
                }
                ImGui::SameLine(0.0f, spacing);
                if (ImGui::Button("Upd", ImVec2(btn_w, 0))) {
                    app.UpdatePersistentCamera(idx);
                }
                ImGui::SameLine(0.0f, spacing);
                if (ImGui::Button("Del", ImVec2(btn_w, 0))) {
                    app.DeletePersistentCamera(idx);
                    ImGui::PopID();
                    break; // storage mutated; abandon rest of this frame
                }

                ImGui::PopID();
            }
        }
    }
    if (ImGui::CollapsingHeader("Selected Renderable")) {
        if (app.selection_state_.selected_deferred_renderable_index != UINT32_MAX) {
            auto renderable = app.scene_->GetRenderables()[app.selection_state_.selected_deferred_renderable_index];
            ImGui::Text("Index: %d", renderable->GetIndex());
            ImGui::Text("Type: %s", ToString(renderable->GetType()).c_str());
            Transform& t = renderable->EditTransform();
            ImGui::InputFloat3("Position", &t.position[0]);
            ImGui::InputFloat3("Rotation", &t.rotation[0]);
            ImGui::InputFloat3("Scale", &t.scale[0]);
            ImGui::Text("AABB: Min(%.2f, %.2f, %.2f) Max(%.2f, %.2f, %.2f)",
                renderable->GetAABB().min.x, renderable->GetAABB().min.y, renderable->GetAABB().min.z,
                renderable->GetAABB().max.x, renderable->GetAABB().max.y, renderable->GetAABB().max.z
            );
            bool hide = renderable->IsVisible();
            ImGui::Checkbox("Visible", &hide);
            renderable->SetVisible(hide);
        } else {
            ImGui::Text("None");
            if (ImGui::Button("Reveal All Hidden")) {
                for (const auto& r : app.scene_->GetRenderables()) {
                    if (r) r->SetVisible(true);
                }
            }
        }
    }
    if (!app.baking_state_.is_baking_mode) {
        if (ImGui::Button("Start Baking")) {
            ops.should_start_baking = true;
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Halt Baking")) {
            app.baking_state_.is_baking_mode = false;
        }
        ImGui::PopStyleColor();
    }
    if (app.baking_state_.is_baking_mode) {
        float fraction = (float)app.baking_state_.baking_frame_index / (float)app.baking_state_.baking_max_num_frames;
        ImGui::Text("Baking Mode: Camera %d / %d, Frame %d / %d",
            app.baking_state_.baking_camera_index + 1, (uint32_t)app.baking_state_.baking_camera_positions.size(),
            app.baking_state_.baking_frame_index + 1, app.baking_state_.baking_max_num_frames
        );
        float remaining_frames = (float)(((int)app.baking_state_.baking_camera_positions.size() - app.baking_state_.baking_camera_index - 1) * app.baking_state_.baking_max_num_frames
            + (app.baking_state_.baking_max_num_frames - app.baking_state_.baking_frame_index));
        float avg_frame_time = cpu_duration;
        float raw_remaining_time_sec = remaining_frames * avg_frame_time;
        static float remaining_time_sec = 0;
        remaining_time_sec = remaining_time_sec * 0.99f + raw_remaining_time_sec * 0.01f;
        float remaining_time_min = remaining_time_sec / 60.0f;
        float remaining_time_hr = remaining_time_min / 60.0f;
        float rest_remaining_time_min = fmod(remaining_time_min, 60.0f);
        float rest_remaining_time_sec = fmod(remaining_time_sec, 60.0f);
        int show_hr = (int)floor(remaining_time_hr);
        int show_min = (int)floor(rest_remaining_time_min);
        int show_sec = (int)floor(rest_remaining_time_sec);
        ImGui::Text("ETA: %02d:%02d:%02d", show_hr, show_min, show_sec);
        ImGui::SameLine();
        ImGui::ProgressBar(fraction, ImVec2(0.0f, 0.0f));
    }
    if (ImGui::CollapsingHeader("Performance")) {
        ImGui::Text("CPU: %3.2f ms (FPS: %3.2f)", cpu_duration * 1000.0, 1.0f / cpu_duration);
        float device_duration = 0.f;
        for (auto & period : time_periods) {
            device_duration += period.duration;
        }
        if (!time_periods.empty()) ImGui::Text("GPU: %.2f ms", device_duration * 1000.0);
        else ImGui::Text("GPU : N/A (Available in Debug build)");
        ImGui::Separator();
        if (RHICmdStats::IsEnabled()) {
            uint32_t num_rhi_commands = 0;
            {
                auto counters = RHICmdStats::Get().GetLastFrameCounters();
                for (auto& counter : counters) {
                    num_rhi_commands += (uint32_t)counter;
                }
            }
            ImGui::Text("RHI Command Throughput: %d", num_rhi_commands);
        } else {
            ImGui::Text("RHI Command Throughput: N/A (Available in Debug build)");
        }
        if (DebugProfIsEnabled()) {
            ImGui::Text("CPU Frame Timed Sections:");
            auto prof_cpu_periods = DebugProfGetSectionStatistics();
            for (auto& period : prof_cpu_periods) {
                ImGui::Text("  %s: %3.2f ms (%5d)", period.second.name.c_str(), double(period.second.time_ns) / 1e6, period.second.call_count);
            }
            DebugProfResetSectionTimes();
        } else {
            ImGui::Text("CPU Frame Timed Sections: N/A (Available in Debug build)");
        }
        ImGui::Separator();
        ImGui::Indent(20);
        if (ImGui::TreeNode("Detailed GPU Profile")) {
            std::function<void(int, int, int)> DrawTree;
            DrawTree = [&](int start, int end, int depth) {
                ImGui::Indent(20);
                int last = start;
                for (int i = start; i < end; i++) {
                    if (time_periods[i].class_names.size() <= depth
                    ||  time_periods[i].class_names[depth] != time_periods[last].class_names[depth]) {
                        if (last != i) {
                            std::string node_name = time_periods[last].class_names[depth];
                            float duration = 0.0f;
                            for (int j = last; j < i; j++) {
                                duration += time_periods[j].duration;
                            }
                            std::string id = node_name;
                            node_name += std::format(" ({:.2f} ms)", duration * 1000);
                            if (ImGui::TreeNode(id.c_str(), "%s", node_name.c_str())) {
                                DrawTree(last, i, depth + 1);
                                ImGui::TreePop();
                            }
                        }
                        if (time_periods[i].class_names.size() <= depth) {
                            std::string node_name = time_periods[i].pass_name;
                            if (node_name.empty()) node_name = "<unnamed>";
                            auto & stat = app.perf_stats_[node_name];
                            float ms = time_periods[i].duration * 1000.0f;
                            stat.min_ms = std::min(stat.min_ms, ms);
                            stat.max_ms = std::max(stat.max_ms, ms);
                            ImGui::Text("%s: %.2f ms (min %.2f / max %.2f)", node_name.c_str(), ms, stat.min_ms, stat.max_ms);
                        }
                        last = i + 1;
                    }
                }
                if (last < end) {
                    std::string node_name = time_periods[last].class_names[depth];
                    float duration = 0.0f;
                    for (int j = last; j < end; j++) {
                        duration += time_periods[j].duration;
                    }
                    std::string id = node_name;
                    node_name += std::format(" ({:.2f} ms)", duration * 1000);
                    if (ImGui::TreeNode(id.c_str(), "%s", node_name.c_str())) {
                        DrawTree(last, end, depth + 1);
                        ImGui::TreePop();
                    }
                }
                ImGui::Unindent(20);
            };
            DrawTree(0, (int)time_periods.size(), 0);
            ImGui::TreePop();
        }
        ImGui::Unindent(20);
    }
}

MI_NAMESPACE_END

