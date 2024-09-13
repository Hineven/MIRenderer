/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "mi/ml.h"

MI_NAMESPACE_BEGIN

void MIFoundation::Start () {
    // Initialize task graph
    // Dispatch threads
    rendering_thread_ = std::make_unique<std::thread>([this]() {
        Run();
    });
}

void MIFoundation::Run () {
    // Main rendering loop
    while(true) {
        // Render a frame
    }
}

void MIFoundation::SynchronizeFrame () {
    // Synchronize a frame (block until the render commands for the previous frame is submitted and the next frame
    // is ready for recording.)
}

MI_NAMESPACE_END