/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "macromc_app.h"

#include <iostream>

#if MI_ENABLE_SHADER_DEBUGGING
#include "infra_impl/infra.h"
#endif

int main(int argc, char** argv) {
    using namespace MI_NAMESPACE;

    macromc::MacroMCStartConfig cfg {};

#if MI_ENABLE_SHADER_DEBUGGING
    auto infra = std::make_unique<MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<MyInfra>(true);
#endif

    try {
        macromc::RunMacroMC(std::move(infra), cfg);
    } catch (const std::exception& e) {
        std::cerr << "MacroMC fatal: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}
