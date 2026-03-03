/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <argparse/argparse.hpp>
#include "viewer_app.h"

using namespace MI_NAMESPACE;

int main (int argc, char** argv) {
    MainLoopStartConfig cfg {};

    cfg.window_width = 1920;
    cfg.window_height = 1080;

    argparse::ArgumentParser program("3D Viewer Application");
    program.add_argument("--width")
        .help("Window width")
        .default_value(cfg.window_width)
        .scan<'u', uint32_t>();
    program.add_argument("--height")
        .help("Window height")
        .default_value(cfg.window_height)
        .scan<'u', uint32_t>();
    program.add_argument("--scene")
        .help("Scene file path (json)")
        .default_value("");

    program.add_argument("--empty")
        .help("Start without loading anything (overrides --scene)")
        .default_value(false)
        .implicit_value(true);
    
    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << program;
        return -1;
    }

    // parse args
    cfg.window_width = program.get<uint32_t>("--width");
    cfg.window_height = program.get<uint32_t>("--height");
    std::string scene_path = program.get<std::string>("--scene");
    bool start_empty = program.get<bool>("--empty");
    cfg.scene_config_path = scene_path;
    cfg.start_empty = start_empty;

#if MI_ENABLE_SHADER_DEBUGGING
    auto infra = std::make_unique<MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<MyInfra>(true);
#endif
    Run3DViewer(std::move(infra), cfg);
    return 0;
}

