#include "viewer_zmq.h"
#include <nlohmann/json.hpp>
#include "core/common.h"
#include "infra_impl/infra.h"

MI_NAMESPACE_BEGIN

ViewerZmqServer::ViewerZmqServer(const Config& cfg) : cfg_(cfg) {}

ViewerZmqServer::~ViewerZmqServer() { Stop(); }

void ViewerZmqServer::Start() {
    if (running_ || !cfg_.enable) return;

    ctx_ = std::make_unique<zmq::context_t>(1);
    router_ = std::make_unique<zmq::socket_t>(*ctx_, zmq::socket_type::router);

    // Set immediate so connect errors don't block
    int immediate = 1;
    router_->set(zmq::sockopt::immediate, immediate);
    router_->set(zmq::sockopt::router_mandatory, 0);
    router_->set(zmq::sockopt::linger, 0);

    try {
        router_->bind(cfg_.bind_endpoint);
    } catch (const zmq::error_t& e) {
        MI_LOG(MIInfraLogType::kError, "ZMQ bind failed: {}", e.what());
        return;
    }

    running_ = true;

    // Long-running poll task that never blocks renderer when no clients.
    poll_task_ = TaskGraph::Get().CreateSimpleTask([this]() {
        this->PollLoop();
    }, TaskPriority::kLow);
    poll_task_->Fire();
}

void ViewerZmqServer::Stop() {
    if (!running_) return;
    running_ = false;

    // Closing sockets will unblock poll
    if (router_) {
        try { router_->close(); } catch(...) {}
    }
    if (ctx_) {
        try { ctx_->close(); } catch(...) {}
    }

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
    while (running_) {
        zmq::pollitem_t items[] = { { static_cast<void*>(*router_), 0, ZMQ_POLLIN, 0 } };
        try {
            zmq::poll(items, 1, std::chrono::milliseconds(10));
        } catch (const zmq::error_t& e) {
            if (!running_) break;
            MI_LOG(MIInfraLogType::kWarning, "ZMQ poll error: {}", e.what());
            continue;
        }

        if ((items[0].revents & ZMQ_POLLIN) != 0) {
            // ROUTER receives: [identity][empty?][payload]
            zmq::message_t identity;
            zmq::message_t payload;

            try {
                auto n1 = router_->recv(identity, zmq::recv_flags::none);

                // Many clients use REQ, which sends only one frame.
                // If there's a second frame that's empty (DEALER), try to read payload next.
                // Peek to see if there's more.
                zmq::message_t maybe_empty;
                bool has_more = router_->get(zmq::sockopt::rcvmore);
                if (has_more) {
                    auto n2 = router_->recv(maybe_empty, zmq::recv_flags::none);
                    (void)n2;
                    has_more = router_->get(zmq::sockopt::rcvmore);
                    if (has_more) {
                        auto n3 = router_->recv(payload, zmq::recv_flags::none);
                        (void)n3;
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
                auto j = json::parse(msg_str);
                std::string cmd = j.value("cmd", "");
                if (cmd == "ping") {
                    reply = { {"ok", true}, {"pong", true} };
                } else if (cmd == "get_status") {
                    reply = { {"ok", true}, {"status", "running"} };
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
                    router_->send(identity, zmq::send_flags::sndmore);
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
}

MI_NAMESPACE_END
