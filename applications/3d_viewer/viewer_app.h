#pragma once

#include <memory>

#include "infra_impl/infra.h"

MI_NAMESPACE_BEGIN

struct MainLoopStartConfig {
    std::string window_name;
    uint32_t window_width;
    uint32_t window_height;
};

// Entry point for running the 3d viewer main loop.
void Run3DViewer(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);


MI_NAMESPACE_END

