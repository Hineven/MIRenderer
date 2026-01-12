#include "viewer_zmq.h"
#include <nlohmann/json.hpp>
#include "core/common.h"
#include "infra_impl/infra.h"

MI_NAMESPACE_BEGIN

ViewerZmqServer::ViewerZmqServer(const Config& cfg) : cfg_(cfg) {}

ViewerZmqServer::~ViewerZmqServer() { Stop(); }

void ViewerZmqServer::Start() {
    if (running_ || !cfg_.enable) return;

    running_ = true;

    // Launch a persistent task in TaskGraph running the ZMQ poll loop.
    // We ensure all ZMQ objects are created and destroyed within the worker thread
    // to strictly facilitate thread-safety and avoid "Bad Address" errors.
    poll_task_ = TaskGraph::Get().CreateSimpleTask([this]() {
        MI_INFO("ViewerZmqServer worker started.");
        this->PollLoop();
        MI_INFO("ViewerZmqServer worker stopped.");
    }, TaskPriority::kLow);
}

void ViewerZmqServer::Stop() {
    if (!running_) return;
    running_ = false;

    // Wait for poll task to finish gracefully
    if (poll_task_) {
        TaskGraph::Get().WaitForTask(poll_task_);
        poll_task_ = nullptr;
    }
}

void ViewerZmqServer::BroadcastInfo(const std::string& info) {
    // Optional: Implement pub socket if needed later.
}

void ViewerZmqServer::PollLoop() {
    using json = nlohmann::json;

    // Create ZMQ context and socket on the stack (thread-local).
    // This avoids "Bad address" errors caused by accessing ZMQ sockets across threads or after improper destruction.
    zmq::context_t ctx;
    zmq::socket_t router;

    try {
        ctx = zmq::context_t(1);
        router = zmq::socket_t(ctx, zmq::socket_type::router);

        int immediate = 1;
        router.set(zmq::sockopt::immediate, immediate);
        router.set(zmq::sockopt::router_mandatory, 0);
        router.set(zmq::sockopt::linger, 0);

        router.bind(cfg_.bind_endpoint);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kError, "ZMQ init/bind failed: {}", e.what());
        return;
    }

    MI_INFO("ViewerZmqServer: Listening on {}", cfg_.bind_endpoint);

    // Use std::vector allocated outside the loop to ensure stable memory address and avoid repeated setup overhead.
    std::vector<zmq::pollitem_t> items(1);
    items[0].socket = router.handle();
    items[0].fd = 0;
    items[0].events = ZMQ_POLLIN;
    items[0].revents = 0;

    while (running_) {
        try {
            zmq::poll(items.data(), 1, std::chrono::milliseconds(10));
        } catch (const zmq::error_t& e) {
            if (!running_) break;
            MI_LOG(MIInfraLogType::kWarning, "ZMQ poll error: {}", e.what());
            // Reset revents on error to be safe
            items[0].revents = 0;
            continue;
        }

        if ((items[0].revents & ZMQ_POLLIN) != 0) {
            // ROUTER receives: [identity][empty?][payload]
            zmq::message_t identity;
            zmq::message_t payload;

            try {
                (void)router.recv(identity, zmq::recv_flags::none);

                // Many clients use REQ, which sends only one frame.
                // If there's a second frame that's empty (DEALER), try to read payload next.
                // Peek to see if there's more.
                zmq::message_t maybe_empty;
                bool has_more = router.get(zmq::sockopt::rcvmore);
                if (has_more) {
                    (void)router.recv(maybe_empty, zmq::recv_flags::none);
                    has_more = router.get(zmq::sockopt::rcvmore);
                    if (has_more) {
                        (void)router.recv(payload, zmq::recv_flags::none);
                    } else {
                        payload = std::move(maybe_empty);
                    }
                } else {
                    // Single frame payload
                    payload = std::move(identity);
                    identity.rebuild();
                }
            } catch (const zmq::error_t& e) {
                MI_LOG(MIInfraLogType::kWarning, "ZMQ recv error: {}", e.what());
                continue;
            }

            std::string id_str(reinterpret_cast<char*>(identity.data()), identity.size());
            std::string msg_str(reinterpret_cast<char*>(payload.data()), payload.size());

            json reply;
            std::vector<uint8_t> binary_reply; // for multipart
            bool multipart = false;

            try {
                auto j = json::parse(msg_str.c_str());
                std::string cmd = j.value("cmd", "");
                if (cmd == "ping") {
                    reply = { {"ok", true}, {"pong", true} };
                } else if (cmd == "get_status") {
                    // Provide camera info and frame index if available
                    if (on_get_status_) {
                        reply = on_get_status_();
                    } else {
                        reply = { {"ok", true}, {"status", "running"} };
                    }
                } else if (cmd == "console_execute") {
                    // console_execute: { cmd: "console_execute", args: { line: "..." } }
                    if (on_console_execute_) {
                        auto line = j["args"].value("line", std::string{});
                        on_console_execute_(line);
                    }
                    reply = { {"ok", true} };
                } else if (cmd == "set_suspended") {
                    // { cmd: "set_suspended", args: { value: true|false } }
                    bool value = j["args"].value("value", false);
                    if (on_set_suspended_) on_set_suspended_(value);
                    reply = { {"ok", true} };
                } else if (cmd == "get_cvar") {
                    if (on_get_cvar_) {
                        auto name = j["args"].value("name", std::string{});
                        reply = on_get_cvar_(name);
                    } else {
                        reply = { {"ok", false}, {"err", "get_cvar_not_supported"} };
                    }
                } else if (cmd == "export_frame") {
                    // Request to export radiance of NEXT frame.
                    // The app callback will orchestrate a single render in suspended mode and return bytes.
                    if (on_export_frame_) {
                        auto r = on_export_frame_();
                        if (r.has_value()) {
                            multipart = true;
                            auto meta = r->meta_json;
                            binary_reply = std::move(r->data);
                            reply = json::parse(meta);
                            reply["ok"] = true;
                        } else {
                            reply = { {"ok", false}, {"err", "export_failed"} };
                        }
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
                if (identity.size() > 0) {
                    router.send(identity, zmq::send_flags::sndmore);
                    router.send(zmq::buffer(s), multipart ? zmq::send_flags::sndmore : zmq::send_flags::none);
                    if (multipart) {
                        router.send(zmq::buffer(binary_reply), zmq::send_flags::none);
                    }
                } else {
                    router.send(zmq::buffer(s), zmq::send_flags::none);
                }
            } catch (const zmq::error_t& e) {
                MI_LOG(MIInfraLogType::kWarning, "ZMQ send error: {}", e.what());
            }
        }
    }
}

MI_NAMESPACE_END
