#include "viewer_commands.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <filesystem>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "viewer_app.h"
#include "core/util/command_line.h"

MI_NAMESPACE_BEGIN
namespace {

inline glm::mat4 ToMat4(const Transform& t) {
    glm::mat4 scale_m = glm::scale(glm::mat4(1.0f), t.scale);
    glm::quat q(t.rotation);
    glm::mat4 rot_m = glm::mat4_cast(q);
    glm::mat4 trans_m = glm::translate(glm::mat4(1.0f), t.position);
    return trans_m * rot_m * scale_m;
}

inline bool TryParseFloats(const std::vector<std::string>& args, size_t start, size_t count, std::array<float, 10>& out) {
    if (start + count > args.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        try {
            out[i] = std::stof(args[start + i]);
        } catch (...) {
            return false;
        }
    }
    return true;
}

} // namespace

void RegisterViewerCommands(ViewerApp& app) {

    // Register viewer commands (app-owned state: pinned CVars and camera).
    auto CompleteCVarId = [](std::string_view prefix) {
        auto &reg = CVarRegistry::GetInstance();
        auto list = reg.GetAllCVars();
        std::vector<std::string> out;
        out.reserve(list.size());
        for (auto *e : list) {
            const auto &id = e->GetId();
            if (prefix.empty() || id.rfind(std::string(prefix), 0) == 0) out.push_back(id);
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    CommandRegistry::Get().MakeAndRegister(
        "pin_cvar",
        {
            CommandTokenSpec::KeywordSet({"pin"}),
            CommandTokenSpec::Free({}, "cvar", CompleteCVarId),
        },
        [&app](const CommandMatchResult &match) {
            if (match.args.size() < 2) {
                MI_WARN("ViewerApp: expected 'pin <cvar>'");
                return;
            }
            const std::string &cvar_id = match.args[1];
            auto &reg = CVarRegistry::GetInstance();
            CVarBase *cvar = reg.GetCVar(cvar_id);
            if (!cvar) {
                MI_WARN("ViewerApp: unknown cvar '{}'", cvar_id);
                return;
            }

            auto it = std::find(app.pinned_cvars_.begin(), app.pinned_cvars_.end(), cvar);
            const bool already_pinned = (it != app.pinned_cvars_.end());
            if (!already_pinned) {
                app.pinned_cvars_.push_back(cvar);
                MI_LOG(MIInfraLogType::kInfo, "Pinned cvar '{}'", cvar_id);
            } else {
                app.pinned_cvars_.erase(it);
                MI_LOG(MIInfraLogType::kInfo, "Unpinned cvar '{}'", cvar_id);
            }
        }
    );

    CommandRegistry::Get().MakeAndRegister(
        "camera_dir",
        {
            CommandTokenSpec::KeywordSet({"c"}),
            CommandTokenSpec::KeywordSet({"dir"}),
            CommandTokenSpec::Free({}, "x"),
            CommandTokenSpec::Free({}, "y"),
            CommandTokenSpec::Free({}, "z"),
        },
        [&app](const CommandMatchResult &match) {
            if (match.args.size() < 5) {
                MI_WARN("ViewerApp: expected 'c dir <x> <y> <z>'");
                return;
            }
            if (!app.view_) {
                MI_WARN("ViewerApp: camera not ready yet");
                return;
            }
            try {
                glm::vec3 dir;
                dir.x = std::stof(match.args[2]);
                dir.y = std::stof(match.args[3]);
                dir.z = std::stof(match.args[4]);
                const float len = glm::length(dir);
                if (len <= 1e-6f) {
                    MI_WARN("ViewerApp: direction length is too small");
                    return;
                }
                app.view_->camera_.direction = dir / len;
            } catch (...) {
                MI_WARN("ViewerApp: invalid float(s) for 'c dir'");
            }
        }
    );

    CommandRegistry::Get().MakeAndRegister(
        "camera_pos",
        {
            CommandTokenSpec::KeywordSet({"c"}),
            CommandTokenSpec::KeywordSet({"pos"}),
            CommandTokenSpec::Free({}, "x"),
            CommandTokenSpec::Free({}, "y"),
            CommandTokenSpec::Free({}, "z"),
        },
        [&app](const CommandMatchResult &match) {
            if (match.args.size() < 5) {
                MI_WARN("ViewerApp: expected 'c pos <x> <y> <z>'");
                return;
            }
            if (!app.view_) {
                MI_WARN("ViewerApp: camera not ready yet");
                return;
            }
            try {
                glm::vec3 pos;
                pos.x = std::stof(match.args[2]);
                pos.y = std::stof(match.args[3]);
                pos.z = std::stof(match.args[4]);
                app.view_->camera_.position = pos;
            } catch (...) {
                MI_WARN("ViewerApp: invalid float(s) for 'c pos'");
            }
        }
    );

    CommandRegistry::Get().MakeAndRegister(
        "camera_fovy",
        {
            CommandTokenSpec::KeywordSet({"c"}),
            CommandTokenSpec::KeywordSet({"fovy"}),
            CommandTokenSpec::Free({}, "fovy"),
        },
        [&app](const CommandMatchResult &match) {
            if (match.args.size() < 3) {
                MI_WARN("ViewerApp: expected 'c fovy <fovy>'");
                return;
            }
            if (!app.view_) {
                MI_WARN("ViewerApp: camera not ready yet");
                return;
            }
            try {
                float fovy = std::stof(match.args[2]);
                app.view_->camera_.fov_Y = fovy;
            } catch (...) {
                MI_WARN("ViewerApp: invalid float for 'c fovy'");
            }
        }
    );

    CommandRegistry::Get().MakeAndRegister(
        "load",
        {
            CommandTokenSpec::KeywordSet({"load"}),
            CommandTokenSpec::Free({}, "abs_path"),
        },
        [&app](const CommandMatchResult &match) {
            if (match.args.size() < 2) {
                MI_WARN("ViewerApp: expected 'load <abs_path>'");
                return;
            }
            std::filesystem::path p(match.args[1]);
            if (p.empty()) {
                MI_WARN("ViewerApp: empty path for load");
                return;
            }
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            bool ok = false;
            std::vector<uint32_t> indices;
            if (ext == ".gltf" || ext == ".glb") {
                ok = app.LoadGLTFAbsolute(p, &indices);
            } else if (ext == ".ply") {
                ok = app.LoadPLYAsGRFAbsolute(p, indices);
            } else {
                MI_WARN("ViewerApp: unsupported extension '{}' for load", ext);
                return;
            }
            if (ok) {
                std::ostringstream oss;
                for (size_t i = 0; i < indices.size(); ++i) {
                    oss << indices[i];
                    if (i + 1 < indices.size()) oss << ",";
                }
                MI_LOG(MIInfraLogType::kInfo, "Loaded {} ({} renderables). Indices: {}",
                    p.string(), indices.size(), oss.str());
            } else {
                MI_WARN("ViewerApp: failed to load {}", p.string());
            }
        }
    );


    CommandRegistry::Get().MakeAndRegister(
        "remove",
        {
            CommandTokenSpec::KeywordSet({"remove"}),
            CommandTokenSpec::Free({}, "renderable_index"),
        },
        [&app](const CommandMatchResult &match) {
            if (match.args.size() < 2) {
                MI_WARN("ViewerApp: expected 'remove <renderable_index>'");
                return;
            }
            uint32_t idx = UINT32_MAX;
            try { idx = static_cast<uint32_t>(std::stoul(match.args[1])); } catch (...) {}
            if (idx == UINT32_MAX) {
                MI_WARN("ViewerApp: invalid renderable index '{}'", match.args[1]);
                return;
            }
            if (app.RemoveRenderableByIndex(idx)) {
                MI_LOG(MIInfraLogType::kInfo, "Removed renderable {}", idx);
            } else {
                MI_WARN("ViewerApp: failed to remove renderable {}", idx);
            }
        }
    );

    CommandRegistry::Get().MakeAndRegister(
        "set_transform",
        {
            CommandTokenSpec::KeywordSet({"set_transform"}),
            CommandTokenSpec::Free({}, "renderable_index"),
            CommandTokenSpec::Free({}, "world_pos_x"),
            CommandTokenSpec::Free({}, "world_pos_y"),
            CommandTokenSpec::Free({}, "world_pos_z"),
            CommandTokenSpec::Free({}, "world_rot_x_euler_deg"),
            CommandTokenSpec::Free({}, "world_rot_y_euler_deg"),
            CommandTokenSpec::Free({}, "world_rot_z_euler_deg"),
            CommandTokenSpec::Free({}, "world_scale_x"),
            CommandTokenSpec::Free({}, "world_scale_y"),
            CommandTokenSpec::Free({}, "world_scale_z"),
        },
        [&app](const CommandMatchResult &match) {
            constexpr size_t kExpected = 11; // cmd + 10 params
            if (match.args.size() < kExpected) {
                MI_WARN("ViewerApp: expected 'set_transform <idx> <px> <py> <pz> <rx_deg> <ry_deg> <rz_deg> <sx> <sy> <sz>'");
                return;
            }
            uint32_t idx = UINT32_MAX;
            try { idx = static_cast<uint32_t>(std::stoul(match.args[1])); } catch (...) {}
            if (idx == UINT32_MAX) {
                MI_WARN("ViewerApp: invalid renderable index '{}'", match.args[1]);
                return;
            }
            std::array<float, 10> vals{};
            if (!TryParseFloats(match.args, 2, 10, vals)) {
                MI_WARN("ViewerApp: set_transform failed to parse float parameters");
                return;
            }
            auto& renderables = app.scene_->GetRenderables();
            if (idx >= renderables.size() || !renderables[idx]) {
                MI_WARN("ViewerApp: renderable index {} not found", idx);
                return;
            }
            auto rend = renderables[idx].Raw();
            auto it = app.renderable_node_lookup_.find(rend);
            if (it == app.renderable_node_lookup_.end() || !it->second) {
                MI_WARN("ViewerApp: renderable {} has no associated scene node", idx);
                return;
            }
            auto node = it->second;
            glm::mat4 parent_world = glm::mat4(1.0f);
            if (node->GetParent()) {
                parent_world = ToMat4(node->GetParent()->GetWorldTransform());
            }
            Transform target_world{};
            target_world.position = {vals[0], vals[1], vals[2]};
            target_world.rotation = glm::radians(glm::vec3(vals[3], vals[4], vals[5]));
            target_world.scale    = {vals[6], vals[7], vals[8]};

            glm::mat4 world_m = ToMat4(target_world);
            glm::mat4 local_m = glm::inverse(parent_world) * world_m;
            Transform local = Transform::FromMatrix(local_m);
            node->SetLocalTransform(local);
            MI_LOG(MIInfraLogType::kInfo, "set_transform applied to renderable {}", idx);
        }
    );
}

MI_NAMESPACE_END
