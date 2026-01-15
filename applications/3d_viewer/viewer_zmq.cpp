#include <coroutine>
#include <nlohmann/json.hpp>
#include <zmq.hpp>

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
        router_ = std::make_unique<zmq::socket_t>(*ctx_, zmq::socket_type::router);

        int immediate = 1;
        router_->set(zmq::sockopt::immediate, immediate);
        router_->set(zmq::sockopt::router_mandatory, 0);
        router_->set(zmq::sockopt::linger, 0);

        router_->bind(cfg_.bind_endpoint);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kError, "ZMQ init/bind failed: {}", e.what());
        return;
    }

    MI_INFO("ViewerZmqServer: Listening on {}", cfg_.bind_endpoint);

    std::vector<zmq::pollitem_t> items(1);
    items[0].socket = router_->handle();
    items[0].fd = 0;
    items[0].events = ZMQ_POLLIN;
    items[0].revents = 0;

    MI_INFO("ViewerZmqServer initialized.");
}

void ViewerZmqServer::Destroy() {
    MI_INFO("ViewerZmqServer destroying...");
}

void ViewerZmqServer::BroadcastInfo(const std::string& info) {
    // Optional: Implement pub socket if needed later.
}

static nlohmann::json ToJson (glm::vec3 v) {
    return nlohmann::json{ v.x, v.y, v.z };
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
            j["value"] = {
                cv->Get()[0], cv->Get()[1]
            };
            break;
        }
        case CVarType::kFloat3: {
            auto* cv = static_cast<CVar<glm::vec3>*>(cvar);
            j["value"] = {
                cv->Get()[0], cv->Get()[1], cv->Get()[2]
            };
            break;
        }
        case CVarType::kFloat4: {
            auto* cv = static_cast<CVar<glm::vec4>*>(cvar);
            j["value"] = {
                cv->Get()[0], cv->Get()[1], cv->Get()[2], cv->Get()[3]
            };
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
    std::vector<zmq::pollitem_t> items(1);
    while (true) {
        {
            items[0] = zmq::pollitem_t{};
            items[0].socket = router_->handle();
            items[0].fd = 0;
            items[0].events = ZMQ_POLLIN;
            items[0].revents = 0;
        }
        try {
            auto num_items = zmq::poll(items.data(), 1, std::chrono::milliseconds(0));
            if (num_items == 0) {
                // No pending events
                break;
            }
        }
        catch (const zmq::error_t& e) {
            MI_LOG(MIInfraLogType::kWarning, "ZMQ poll error: {}", e.what());
            break;
        }

        if ((items[0].revents & ZMQ_POLLIN) != 0) {
            // ROUTER receives: [identity][empty?][payload]
            zmq::message_t payload;
            bool send_empty_frame = false;
            if (!last_message_identity_) {
                last_message_identity_ = std::make_unique<zmq::message_t>();
            }
            try {
                (void)router_->recv(*last_message_identity_, zmq::recv_flags::none);

                // Many clients use REQ, which sends only one frame.
                // If there's a second frame that's empty (DEALER), try to read payload next.
                // Peek to see if there's more.
                zmq::message_t maybe_empty;
                bool has_more = router_->get(zmq::sockopt::rcvmore);
                if (has_more) {
                    (void)router_->recv(maybe_empty, zmq::recv_flags::none);
                    has_more = router_->get(zmq::sockopt::rcvmore);
                    if (has_more) {
                        (void)router_->recv(payload, zmq::recv_flags::none);
                        // If maybe_empty is actually empty, we should echo it back for REQ sockets
                        if (maybe_empty.size() == 0) send_empty_frame = true;
                    } else {
                        payload = std::move(maybe_empty);
                    }
                } else {
                    // Single frame payload
                    payload = std::move(*last_message_identity_);
                    last_message_identity_->rebuild();
                }
            } catch (const zmq::error_t& e) {
                MI_LOG(MIInfraLogType::kWarning, "ZMQ recv error: {}", e.what());
                continue;
            }

            std::string msg_str(reinterpret_cast<char*>(payload.data()), payload.size());

            json reply;
            std::vector<uint8_t> binary_reply;
            bool multipart = false;

            try {
                auto j = json::parse(msg_str);
                std::string cmd = j.value("cmd", "");
                if (cmd == "ping") {
                    reply = { {"ok", true}, {"pong", true} };
                } else if (cmd == "get_status") {
                    if (viewer_) {
                        ViewerApp::ViewerStatus status {};
                        {
                            auto status_fut = viewer_->GetStatus();
                        }
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
                        reply = { {"ok", false} };
                    }
                } else if (cmd == "console_execute") {
                    if (viewer_) {
                        auto line = j["args"].value("line", std::string{});
                        auto match = CommandRegistry::Get().Match(line);
                        if (match.has_value()) {
                            if (match->kind == CommandMatchKind::kFull) {
                                match->command->Execute(match.value());
                                reply = { {"ok", true} };
                            } else {
                                reply = { {"ok", false}, {"err", "bad command"} };
                            }
                        } else {
                            reply = { {"ok", false}, {"err", "command parse error"}};
                        }
                    } else {
                        reply = { {"ok", false}, {"err", "no_viewer"} };
                    }
                } else if (cmd == "set_suspended") {
                    bool value = j["args"].value("value", false);
                    if (viewer_) viewer_->SetSuspended(value);
                    reply = { {"ok", true} };
                } else if (cmd == "get_cvar") {
                    if (viewer_) {
                        auto name = j["args"].value("name", std::string{});
                        {
                            auto cvar = CVarRegistry::GetInstance().GetCVar(name);
                            if (cvar) {
                                reply = { {"ok", true}, {"cvar", GetCVarJson(cvar)}};
                            } else {
                                reply = { {"ok", false}, {"err", "cvar_not_found"} };
                            }
                        }
                    } else {
                        reply = { {"ok", false}, {"err", "no_viewer"} };
                    }
                } else if (cmd == "render_and_export_current_frame") {
                    if (viewer_) {
                        auto args = j.value("args", json::object());
                        auto types = args.value("types", std::vector<std::string>{"radiance"});
                        // Break from the loop. Let the viewer take care of the export.
                        return types;
                    } else {
                        reply = { {"ok", false}, {"err", "export_not_supported"} };
                    }
                } else {
                    reply = { {"ok", false}, {"err", "unknown_cmd"} };
                }
            } catch (const std::exception& e) {
                reply = { {"ok", false}, {"err", std::string("json_parse_error: ") + e.what()} };
            }

            auto s = reply.dump();

            try {
                if (last_message_identity_->size() > 0) {
                    router_->send(*last_message_identity_, zmq::send_flags::sndmore);
                    if (send_empty_frame) {
                        router_->send(zmq::message_t(), zmq::send_flags::sndmore);
                    }
                    router_->send(zmq::buffer(s), multipart ? zmq::send_flags::sndmore : zmq::send_flags::none);
                    if (multipart) {
                        router_->send(zmq::buffer(binary_reply), zmq::send_flags::none);
                    }
                } else {
                    router_->send(zmq::buffer(s), zmq::send_flags::none);
                }
            } catch (const zmq::error_t& e) {
                MI_LOG(MIInfraLogType::kWarning, "ZMQ send error: {}", e.what());
            }
        }
    }

    return {};
}

void ViewerZmqServer::ReplyExportedFrame() {
    using nlohmann::json;
    json reply {};
    bool multipart {};
    std::vector<std::byte> binary_reply;
    {
        auto results = viewer_->GetAndClearExportedFrameResults();
        json exports = json::array();
        size_t total_size = 0;
        for (const auto& res : results) {
            json item = {
                {"name",  res.name},
                {"width", res.width},
                {"height", res.height},
                {"format", ToString(res.format)},
                {"size_bytes", res.bytes.size()}
            };
            exports.push_back(item);
            total_size += res.bytes.size();
        }
        reply = { {"ok", true}, {"exports", exports} };
        if (total_size > 0) {
            multipart = true;
            binary_reply.reserve(total_size);
            for (const auto& res : results) {
                binary_reply.insert(binary_reply.end(),
                    res.bytes.begin(), res.bytes.end());
            }
        }
    }

    auto s = reply.dump();

    // Send reply for frame export request
    try {
        if (last_message_identity_->size() > 0) {
            router_->send(*last_message_identity_, zmq::send_flags::sndmore);
            router_->send(zmq::buffer(s), multipart ? zmq::send_flags::sndmore : zmq::send_flags::none);
            if (multipart) {
                router_->send(zmq::buffer(binary_reply), zmq::send_flags::none);
            }
        } else {
            router_->send(zmq::buffer(s), zmq::send_flags::none);
        }
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kWarning, "ZMQ send error: {}", e.what());
    }
}

MI_NAMESPACE_END
