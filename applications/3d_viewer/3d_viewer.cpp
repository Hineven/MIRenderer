/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "viewer_app.h"

using namespace MI_NAMESPACE;

int main () {
    MainLoopStartConfig cfg {};
    cfg.window_width = 1920;
    cfg.window_height = 1080;

#if MI_ENABLE_SHADER_DEBUGGING
    auto infra = std::make_unique<MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<MyInfra>(true);
#endif
    Run3DViewer(std::move(infra), cfg);
    return 0;
}

