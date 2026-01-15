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

// Simple protocol:
// - Python connects via REQ/DEALER to tcp://127.0.0.1:5557
// - Messages are JSON (UTF-8). First frame is the payload.
// - Examples: {"cmd":"ping"}, {"cmd":"render","params":{...}}, {"cmd":"get_frame"}
// - Router replies with {"ok":true,...} or {"ok":false,"err":"..."}
// Routing id frame is managed by ROUTER automatically.

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

    // Called by the viewer to reply with exported frame data. Call this once per export request.
    void ReplyExportedFrame();

    // Submit a one-shot notification to clients (best-effort). Optional.
    void BroadcastInfo(const std::string& info);


private:

    std::unique_ptr<::zmq::context_t> ctx_ {};
    std::unique_ptr<::zmq::socket_t> router_ {};

    std::unique_ptr<::zmq::message_t> last_message_identity_ {};

    ViewerApp * viewer_ {};

    Config cfg_{};
};

MI_NAMESPACE_END
