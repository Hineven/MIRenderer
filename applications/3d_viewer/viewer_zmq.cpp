#include <coroutine>
#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include <filesystem>

#include "viewer_zmq.h"

#include "viewer_app.h"
#include "core/common.h"
#include "core/util/command_line.h"
#include "infra_impl/infra.h"

MI_NAMESPACE_BEGIN

ViewerZmqServer::ViewerZmqServer(ViewerApp* viewer, const Config& cfg) : viewer_(viewer), cfg_(cfg) {}

ViewerZmqServer::~ViewerZmqServer() { Destroy(); }

void ViewerZmqServer::Initialize() {
    if (!cfg_.enable) return;

    try {
        ctx_ = std::make_unique<zmq::context_t>(1);
        rep_ = std::make_unique<zmq::socket_t>(*ctx_, zmq::socket_type::rep);

        int immediate = 1;
        rep_->set(zmq::sockopt::immediate, immediate);
        rep_->set(zmq::sockopt::linger, 0);

        rep_->bind(cfg_.bind_endpoint);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kError, "ZMQ init/bind failed: {}", e.what());
        return;
    }

    MI_INFO("ViewerZmqServer: Listening on {}", cfg_.bind_endpoint);
    MI_INFO("ViewerZmqServer initialized.");
}

void ViewerZmqServer::Destroy() {
    MI_INFO("ViewerZmqServer destroying...");
    try {
        if (rep_) rep_->close();
    } catch (const zmq::error_t&) {
    }
    rep_.reset();
    ctx_.reset();
    has_pending_export_reply_ = false;
    pending_export_types_.clear();
    has_pending_reload_reply_ = false;
    reload_shaders_requested_ = false;
}

void ViewerZmqServer::BroadcastInfo(const std::string& info) {
    MI_LOG(MIInfraLogType::kWarning, "BroadcastInfo not supported in REQ/REP mode. msg={}", info);
}

static nlohmann::json ToJson (glm::vec3 v) {
    return nlohmann::json{ v.x, v.y, v.z };
}

static const char* ToConsoleLogTypeString(ViewerImGuiConsole::ConsoleLogType type) {
    switch (type) {
        case ViewerImGuiConsole::ConsoleLogType::kInfo:
            return "info";
        case ViewerImGuiConsole::ConsoleLogType::kWarning:
            return "warning";
        case ViewerImGuiConsole::ConsoleLogType::kError:
            return "error";
        case ViewerImGuiConsole::ConsoleLogType::kRaw:
            return "raw";
        default:
            return "raw";
    }
}

static nlohmann::json GetConsoleLogJson(const ViewerImGuiConsole::ConsoleLogEntry& entry) {
    using namespace std::chrono;
    const auto timestamp_ms = duration_cast<milliseconds>(entry.timestamp.time_since_epoch()).count();
    return nlohmann::json{
        {"text", entry.text},
        {"location", entry.location},
        {"type", ToConsoleLogTypeString(entry.type)},
        {"count", entry.count},
        {"timestamp_ms", timestamp_ms}
    };
}

static nlohmann::json GetCVarJson(CVarBase* cvar) {
    nlohmann::json j;
    j["id"] = cvar->GetId();
    j["type"] = ToString(cvar->GetType());
    j["description"] = cvar->GetDescription();
    switch (cvar->GetType()) {
        case CVarType::kBool: {
            auto* cv = static_cast<CVar<bool>*>(cvar);
            j["value"] = cv->Get();
            break;
        }
        case CVarType::kInt: {
            auto* cv = static_cast<CVar<int>*>(cvar);
            j["value"] = cv->Get();
            break;
        }
        case CVarType::kFloat: {
            auto* cv = static_cast<CVar<float>*>(cvar);
            j["value"] = cv->Get();
            break;
        }
        case CVarType::kFloat2: {
            auto* cv = static_cast<CVar<glm::vec2>*>(cvar);
            j["value"] = { cv->Get()[0], cv->Get()[1] };
            break;
        }
        case CVarType::kFloat3: {
            auto* cv = static_cast<CVar<glm::vec3>*>(cvar);
            j["value"] = { cv->Get()[0], cv->Get()[1], cv->Get()[2] };
            break;
        }
        case CVarType::kFloat4: {
            auto* cv = static_cast<CVar<glm::vec4>*>(cvar);
            j["value"] = { cv->Get()[0], cv->Get()[1], cv->Get()[2], cv->Get()[3] };
            break;
        }
        case CVarType::kString: {
            auto* cv = static_cast<CVar<std::string>*>(cvar);
            j["value"] = cv->Get();
            break;
        }
        default:
            j["value"] = nullptr;
            break;
    }
    return j;
}

std::vector<std::string> ViewerZmqServer::PollEvents() {
    using json = nlohmann::json;
    if (!rep_) return {};
    if (has_pending_export_reply_ || has_pending_reload_reply_) return {};

    std::vector<zmq::pollitem_t> items(1);
    items[0] = zmq::pollitem_t{};
    items[0].socket = rep_->handle();
    items[0].fd = 0;
    items[0].events = ZMQ_POLLIN;
    items[0].revents = 0;

    try {
        auto num_items = zmq::poll(items.data(), 1, std::chrono::milliseconds(0));
        if (num_items == 0) return {};
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kWarning, "ZMQ poll error: {}", e.what());
        return {};
    }

    if ((items[0].revents & ZMQ_POLLIN) == 0) return {};

    zmq::message_t payload;
    try {
        (void)rep_->recv(payload, zmq::recv_flags::none);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kWarning, "ZMQ recv error: {}", e.what());
        return {};
    }

    std::string msg_str(reinterpret_cast<char*>(payload.data()), payload.size());
    json reply;
    try {
        json j = json::parse(msg_str, nullptr, false);
        if (j.is_discarded()) throw std::runtime_error("json_parse_error");
        std::string cmd = j.value("cmd", "");

        if (cmd == "ping") {
            reply = { {"ok", true}, {"pong", true} };
        } else if (cmd == "get_status") {
            if (viewer_) {
                auto status = viewer_->GetStatus();
                auto camera = nlohmann::json{
                    {"position", ToJson(status.camera.position)},
                    {"direction", ToJson(status.camera.direction)},
                    {"up", ToJson(status.camera.up)},
                    {"fov_y", status.camera.fov_Y},
                    {"near_plane", status.camera.near_plane},
                    {"far_plane", status.camera.far_plane}
                };
                auto status_json = nlohmann::json{
                    {"frame_index", status.frame_index},
                    {"is_suspended", status.is_suspended},
                    {"camera", camera}
                };
                reply = { {"ok", true}, {"status", status_json} };
            } else {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            }
        } else if (cmd == "console_execute") {
            if (viewer_) {
                auto line = j["args"].value("line", std::string{});
                auto match = CommandRegistry::Get().Match(line);
                if (match.has_value() && match->kind == CommandMatchKind::kFull) {
                    match->command->Execute(match.value());
                    reply = { {"ok", true} };
                } else {
                    reply = { {"ok", false}, {"err", "bad command"} };
                }
            } else {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            }
        } else if (cmd == "set_suspended") {
            bool value = j["args"].value("value", false);
            if (viewer_) viewer_->SetSuspended(value);
            reply = { {"ok", true} };
        } else if (cmd == "get_cvar") {
            auto name = j["args"].value("name", std::string{});
            auto cvar = CVarRegistry::GetInstance().GetCVar(name);
            if (cvar) {
                reply = { {"ok", true}, {"cvar", GetCVarJson(cvar)} };
            } else {
                reply = { {"ok", false}, {"err", "cvar_not_found"} };
            }
        } else if (cmd == "get_recent_logs") {
            if (!viewer_) {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            } else {
                auto max_count = j["args"].value("count", 20u);
                auto logs = viewer_->GetLatestUniqueLogs(max_count);
                nlohmann::json logs_json = nlohmann::json::array();
                for (const auto& log : logs) {
                    logs_json.push_back(GetConsoleLogJson(log));
                }
                reply = { {"ok", true}, {"logs", logs_json} };
            }
        } else if (cmd == "render_and_export_current_frame") {
            auto args = j.value("args", json::object());
            auto types = args.value("types", std::vector<std::string>{"radiance"});
            pending_export_types_ = types;
            has_pending_export_reply_ = true;
            // Request one-frame rendering.
            viewer_->one_frame_rendering_requested_ = true;
            return pending_export_types_;
        } else if (cmd == "reload_shaders") {
            if (!viewer_) {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            } else {
                reload_shaders_requested_ = true;
                has_pending_reload_reply_ = true;
                return {};
            }
        } else if (cmd == "load_gltf_abs_path") {
            auto path_str = j["args"].value("path", std::string{});
            if (!viewer_) {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            } else if (path_str.empty()) {
                reply = { {"ok", false}, {"err", "missing_path"} };
            } else {
                std::vector<uint32_t> indices;
                bool ok = viewer_->LoadGLTFAbsolute(std::filesystem::path(path_str), &indices);
                reply = { {"ok", ok} };
                if (ok) reply["renderable_indices"] = indices; else reply["err"] = "load_failed";
            }
        } else if (cmd == "load_ply_abs_path") {
            auto path_str = j["args"].value("path", std::string{});
            if (!viewer_) {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            } else if (path_str.empty()) {
                reply = { {"ok", false}, {"err", "missing_path"} };
            } else {
                std::vector<uint32_t> indices;
                bool ok = viewer_->LoadPLYAsGRFAbsolute(std::filesystem::path(path_str), indices);
                reply = { {"ok", ok} };
                if (ok) reply["renderable_indices"] = indices; else reply["err"] = "load_failed";
            }
        } else if (cmd == "remove_renderable_node") {
            uint32_t idx = j["args"].value("index", UINT32_MAX);
            if (!viewer_) {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            } else if (idx == UINT32_MAX) {
                reply = { {"ok", false}, {"err", "missing_index"} };
            } else {
                bool ok = viewer_->RemoveRenderableNodeByIndex(idx);
                reply = { {"ok", ok} };
                if (!ok) reply["err"] = "remove_failed";
            }
        } else if (cmd == "load_scene_config") {
            if (!viewer_) {
                reply = { {"ok", false}, {"err", "no_viewer"} };
            } else {
                auto args = j.value("args", json::object());
                const auto scene_json_str = args.value("json", std::string{});
                const auto scene_path_str = args.value("path", std::string{});

                if (scene_json_str.empty() && scene_path_str.empty()) {
                    reply = { {"ok", false}, {"err", "missing_json_or_path"} };
                } else {
                    bool ok = false;
                    std::string error;
                    if (!scene_path_str.empty()) {
                        const std::filesystem::path scene_path(scene_path_str);
                        if (!scene_path.is_absolute()) {
                            reply = { {"ok", false}, {"err", "path_must_be_absolute"} };
                        } else {
                            ok = viewer_->LoadSceneFromConfigAbsolutePath(scene_path, &error, true);
                            reply = { {"ok", ok} };
                            if (!ok) reply["err"] = error.empty() ? "load_scene_failed" : error;
                        }
                    } else {
                        ok = viewer_->LoadSceneFromConfigJsonString(scene_json_str, &error, true);
                        reply = { {"ok", ok} };
                        if (!ok) reply["err"] = error.empty() ? "load_scene_failed" : error;
                    }
                }
            }
        } else {
            reply = { {"ok", false}, {"err", "unknown_cmd"} };
        }
    } catch (const std::exception& e) {
        reply = { {"ok", false}, {"err", std::string("json_parse_error: ") + e.what()} };
    }

    auto s = reply.dump();
    try {
        rep_->send(zmq::buffer(s), zmq::send_flags::none);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kWarning, "ZMQ send error: {}", e.what());
    }

    return {};
}

void ViewerZmqServer::ReplyExportedFrame() {
    if (!rep_ || !has_pending_export_reply_) return;

    using nlohmann::json;
    std::vector<ViewerApp::ExportedRenderResult> results;
    {
        results = viewer_->GetAndClearExportedFrameResults();
    }

    json exports = json::array();
    for (const auto& res : results) {
        json item = {
            {"name",  res.name},
            {"width", res.width},
            {"height", res.height},
            {"format", ToString(res.format)},
            {"bytes_per_pixel", GetPixelFormatBytesPerPixel(res.format)},
            {"size_bytes", res.bytes.size()}
        };
        exports.push_back(item);
    }
    json reply_meta = { {"ok", true}, {"exports", exports} };

    auto meta_str = reply_meta.dump();

    try {
        if (!results.empty()) {
            rep_->send(zmq::buffer(meta_str), zmq::send_flags::sndmore);
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& res = results[i];
                auto flags = (i + 1 == results.size()) ? zmq::send_flags::none : zmq::send_flags::sndmore;
                rep_->send(zmq::buffer(res.bytes), flags);
            }
        } else {
            rep_->send(zmq::buffer(meta_str), zmq::send_flags::none);
        }
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kWarning, "ZMQ send error: {}", e.what());
    }

    has_pending_export_reply_ = false;
    pending_export_types_.clear();
}

bool ViewerZmqServer::ConsumeReloadShadersRequest() {
    if (!reload_shaders_requested_) {
        return false;
    }
    reload_shaders_requested_ = false;
    return true;
}

void ViewerZmqServer::ReplyReloadShaders(bool ok, const std::string& err) {
    if (!rep_ || !has_pending_reload_reply_) return;

    nlohmann::json reply = {
        {"ok", ok}
    };
    if (!ok && !err.empty()) {
        reply["err"] = err;
    }

    auto reply_str = reply.dump();
    try {
        rep_->send(zmq::buffer(reply_str), zmq::send_flags::none);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kWarning, "ZMQ send error: {}", e.what());
    }

    has_pending_reload_reply_ = false;
}

MI_NAMESPACE_END
