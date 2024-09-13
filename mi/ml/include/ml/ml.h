/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_MI_H
#define MIRENDERER_MI_H
#include "core/common.h"
#include "core/infra.h"
#include "ml/ml_fwd.h"

MI_NAMESPACE_BEGIN

struct MIMainLoopStartConfig {

};

class MainLoop {
public:
    // Start the main loop
    void Start () ;

    // Synchronize a frame (block until the render commands for the previous frame is submitted and the next frame
    // is ready for recording.)
    void SynchronizeFrame () ;

protected:
    void Run () ;

    std::unique_ptr<std::thread> rendering_thread_;

};

std::unique_ptr<MIFoundation> CreateRenderer (std::unique_ptr<MIInfraInterface> infra);

MI_NAMESPACE_END

#endif //MIRENDERER_MI_H
