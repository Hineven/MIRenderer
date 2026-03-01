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
    
    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << program;
        return -1;
    }

#if MI_ENABLE_SHADER_DEBUGGING
    auto infra = std::make_unique<MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<MyInfra>(true);
#endif
    Run3DViewer(std::move(infra), cfg);
    return 0;
}

