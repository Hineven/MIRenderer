/*
 * Project Project: main.cpp
 * Created: 2024/6/27
 * This program uses MulanPSL2. See LICENSE for more.
 */

#include <cstdio>
#include <fstream>

#include "core/infra.h"
#include "mi/ml.h"

int main () {
    printf("Hello, world!\n");
    mi::MIInfraInterface * infra_ptr;
    // Fake an infra interface
    std::unique_ptr<mi::MIInfraInterface> infra(infra_ptr);
    auto renderer = mi::CreateRenderer(std::move(infra));
    renderer->Start();
    while(true) {
        renderer->SynchronizeFrame();
    }
    return 0;
}