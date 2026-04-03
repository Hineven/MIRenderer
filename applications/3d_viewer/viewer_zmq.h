#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#include <functional>

#include <nlohmann/json.hpp>
#include <core/task.h>
#include "fwd.h"

namespace zmq {
    class context_t;
    class socket_t;
    class message_t;
};

MI_NAMESPACE_BEGIN

// Simple protocol (single client):
// - Server: ZMQ REP at cfg.bind_endpoint
// - Client: ZMQ REQ
// - Request: single JSON frame {"cmd": string, "args": {}}
// - Reply: JSON meta frame (ok/err) optionally followed by binary frames (for exports)
// - One recv -> one send; render_and_export_current_frame defers reply until ReplyExportedFrame
class ViewerZmqServer {
public:
    struct Config {
        std::string bind_endpoint = "tcp://127.0.0.1:25957"; // localhost only for safety
        bool enable = true; // allow disabling via cvar later
    };

    explicit ViewerZmqServer(ViewerApp * viewer, const Config& cfg);
    ~ViewerZmqServer();

    void Initialize();
    void Destroy();

    // Called by the viewer each frame. Returns list of requested export types to be processed by the viewer.
    std::vector<std::string> PollEvents();

    // Called by the viewer to reply with exported frame data for the last export request.
    void ReplyExportedFrame();
    bool ConsumeReloadShadersRequest();
    void ReplyReloadShaders(bool ok, const std::string& err = {});

    // Submit a one-shot notification to clients (best-effort). Optional.
    void BroadcastInfo(const std::string& info);

private:
    std::unique_ptr<::zmq::context_t> ctx_ {};
    std::unique_ptr<::zmq::socket_t> rep_ {};

    bool has_pending_export_reply_ = false;
    std::vector<std::string> pending_export_types_ {};
    bool has_pending_reload_reply_ = false;
    bool reload_shaders_requested_ = false;

    ViewerApp * viewer_ {};

    Config cfg_{};
};

MI_NAMESPACE_END
