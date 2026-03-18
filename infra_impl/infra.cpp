/*
 * Created: 2024/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#include "infra_impl/infra.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

MyInfra::MyInfra(bool find_resource_directory, std::string resource_directory) {
    if (!find_resource_directory) {
        resource_directory_ = std::filesystem::path(resource_directory);
        if (resource_directory_.empty()) {
            resource_directory_ = std::filesystem::current_path() / "resources";
        }
    }
}

std::filesystem::path MyInfra::GetResourceDirectory() {
    return resource_directory_;
}

std::filesystem::path MyInfra::GetTempDirectory() {
    return temp_directory_;
}



void MyInfra::Init() {
	//记录启动时间
    start_time_ = std::chrono::high_resolution_clock::now();

    //从当前目录开始，逐级向上查找，直到找到包含标识文件"mi_renderer_identity"的目录，在此目录下拿到resources目录
    if (resource_directory_ == "") {
        // Try to find the resource directory via mi_renderer_identity file.
        auto directory = std::filesystem::current_path();
        while (true) {
            if (std::filesystem::exists(directory / "mi_renderer_identity")) {
                resource_directory_ = directory / "resources";
                LogMessage(MIInfraLogType::kInfo, "Found resource directory: " + resource_directory_.string());
                break;
            }
            if (directory == directory.parent_path()) {
                LogMessage(MIInfraLogType::kError, "Failed to find resource directory. Defaulting to current path.");
                resource_directory_ = std::filesystem::current_path() / "resources";
                break;
            }
            directory = directory.parent_path();
        }
    }
    //没有就自己创建
    if (!std::filesystem::exists(resource_directory_)) {
        LogMessage(MIInfraLogType::kInfo, "Creating resource directory: " + resource_directory_.string());
        std::filesystem::create_directory(resource_directory_);
    } else {
        LogMessage(MIInfraLogType::kInfo, "Resource directory: " + resource_directory_.string());
    }
    if (temp_directory_ == "") {
        temp_directory_ =
                std::filesystem::current_path() / "temp";
    }
    if (!std::filesystem::exists(temp_directory_)) {
        LogMessage(MIInfraLogType::kInfo, "Creating temp directory: " + temp_directory_.string());
        std::filesystem::create_directory(temp_directory_);
    } else {
        LogMessage(MIInfraLogType::kInfo, "Temp directory: " + temp_directory_.string());
    }

    // Query for the number of logical cores
    limits_.max_high_performance_thread_count = std::thread::hardware_concurrency();
    limits_.max_low_performance_thread_count = 0;
    // Unlimited memory usage and auto GPU selection by default

    // Kick off file io threads
    KickOffFIOThreads();
}

void MyInfra::Shutdown() {
    // Clear resource cache
    {
        std::lock_guard lock(resource_cache_mutex_);
        resource_cache_.clear();
    }

    // Stop and block wait file io thread
    StopAndBlockWaitFIOThreads();

    // Free compiler contexts
    DestroyHLSLCompilerContexts();

}

// Misc


std::optional<std::unique_ptr<std::thread>>
MyInfra::LaunchThread([[maybe_unused]] ThreadPerformanceType perf_type, std::function<void()> thread_func) {

    return std::make_unique<std::thread>(thread_func);
}

uint32_t MyInfra::GenerateSeed() {
    return seed_identifier_ ++;
}

float MyInfra::GetTimeSinceStart() {
    return std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - start_time_).count();
}

void MyInfra::ProfileStart([[maybe_unused]] const std::string &name) {
    // Do nothing
}

void MyInfra::ProfileEnd([[maybe_unused]] const std::string &name) {
    // Do nothing
}

void MyInfra::AddProfileTime([[maybe_unused]] const std::string &name, [[maybe_unused]] float time) {
    // Do nothing
}

void MyInfra::LogMessage(MIInfraLogType level, const std::string &message, const std::string &location) {
    std::lock_guard guard(log_mutex_);
    std::string level_str;
    std::string color_start; // Color code at start
    std::string color_reset = "\033[0m"; // Reset color

    switch(level) {
        case MIInfraLogType::kInfo:
            level_str = "Info";
            color_start = "\033[0m"; // Default color
            break;
        case MIInfraLogType::kWarning:
            level_str = "Warning";
            color_start = "\033[33m"; // Yellow
            break;
        case MIInfraLogType::kError:
            level_str = "Error";
            color_start = "\033[31m"; // Red
            break;
    }

#ifdef _WIN32
    // 在Windows上启用ANSI支持
    static bool initialized = false;
    if (!initialized) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
        initialized = true;
    }
#endif

    if (log_callback_) {
        log_callback_(level, message, location);
    }

    std::cout << color_start << "[" << level_str << "] " << message;
    if (!location.empty()) {
        std::cout << " (" << location << ")";
    }
    std::cout << color_reset << std::endl;
}

void MyInfra::SetLogCallback(MIInfraLogCallback callback) {
    log_callback_ = std::move(callback);
}

void MyInfra::OnFrameBegin() {
    MIInfraInterface::OnFrameBegin();
    // Do nothing
}

void MyInfra::OnFrameRHISubmit() {
    MIInfraInterface::OnFrameRHISubmit();
    // Do nothing
}

static std::unique_ptr<MIInfraInterface> G_Inftra;

MIInfraInterface & GetInfra() {
    return *G_Inftra;
}

bool HasInfra() {
    return G_Inftra != nullptr;
}

void TransferInfra(std::unique_ptr<MIInfraInterface> &&infra) {
    G_Inftra = std::move(infra);
}

void DestroyInfra() {
    G_Inftra.reset();
}

MI_NAMESPACE_END


