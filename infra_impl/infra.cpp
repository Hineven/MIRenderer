/*
 * Created: 2024/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <iostream>
#include "infra_impl/infra.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

void MyInfra::Init() {
    start_time_ = std::chrono::high_resolution_clock::now();

    // Create / Get directories
    resource_directory_ =
            std::filesystem::current_path() / "resources";
    if (!std::filesystem::exists(resource_directory_)) {
        LogMessage(MIInfraLogType::kInfo, "Creating resource directory: " + resource_directory_.string());
        std::filesystem::create_directory(resource_directory_);
    } else {
        LogMessage(MIInfraLogType::kInfo, "Resource directory: " + resource_directory_.string());
    }

    // Query for the number of logical cores
    limits_.max_high_performance_thread_count = std::thread::hardware_concurrency();
    limits_.max_low_performance_thread_count = 0;
    // Unlimited memory usage and auto GPU selection by default

    // Kick off file io threads
    KickOffFIOThreads();
}

void MyInfra::Shutdown() {
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

void MyInfra::LogMessage(MIInfraLogType level, const std::string &message) {
    // Just print to console
    std::string level_str;
    switch(level) {
        case MIInfraLogType::kInfo:
            level_str = "Info";
            break;
        case MIInfraLogType::kWarning:
            level_str = "Warning";
            break;
        case MIInfraLogType::kError:
            level_str = "Error";
            break;
    }
    std::cout << "[" << level_str << "] " << message << std::endl;
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

void TransferInfra(std::unique_ptr<MIInfraInterface> &&infra) {
    G_Inftra = std::move(infra);
}

void DestroyInfra() {
    G_Inftra.reset();
}

MI_NAMESPACE_END


