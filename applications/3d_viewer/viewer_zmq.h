#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <optional>
#include <functional>

#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include "core/task.h"

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

    // Export response payload: JSON metadata and raw bytes.
    struct ExportPayload {
        std::string meta_json;
        std::vector<uint8_t> data;
    };

    explicit ViewerZmqServer(const Config& cfg);
    ~ViewerZmqServer();

    // Launch a persistent task in TaskGraph running the ZMQ poll loop.
    void Start();
    void Stop();

    bool IsRunning() const { return running_; }

    // Callbacks to integrate with the viewer app
    void SetOnConsoleExecute(std::function<void(const std::string&)> cb) { on_console_execute_ = std::move(cb); }
    void SetOnSetSuspended(std::function<void(bool)> cb) { on_set_suspended_ = std::move(cb); }
    void SetOnExportFrame(std::function<std::optional<ExportPayload>()> cb) { on_export_frame_ = std::move(cb); }
    void SetOnGetStatus(std::function<nlohmann::json()> cb) { on_get_status_ = std::move(cb); }
    void SetOnGetCVar(std::function<nlohmann::json(const std::string&)> cb) { on_get_cvar_ = std::move(cb); }

    // Submit a one-shot notification to clients (best-effort). Optional.
    void BroadcastInfo(const std::string& info);

private:
    void PollLoop();

    Config cfg_{};
    std::atomic<bool> running_{false};


    // TaskGraph integration
    TaskRef poll_task_{}; // created as a long-running task

    // App-provided callbacks
    std::function<void(const std::string&)> on_console_execute_;
    std::function<void(bool)> on_set_suspended_;
    std::function<std::optional<ExportPayload>()> on_export_frame_;
    std::function<nlohmann::json()> on_get_status_;
    std::function<nlohmann::json(const std::string&)> on_get_cvar_;
};

MI_NAMESPACE_END
