/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_ML_H
#define MIRENDERER_ML_H
#include "core/common.h"
#include "core/infra.h"
#include "ml/ml_fwd.h"

MI_NAMESPACE_BEGIN

struct MainLoopStartConfig {

};

class MainLoop {
public:
    // Start the main loop
    void Start () ;

    // Synchronize a frame (block until the render commands for the previous frame is submitted and the next frame
    // is ready for recording.)
    void SynchronizeFrame () ;

protected:

    MainLoop (std::unique_ptr<MIInfraInterface> infra) : infra_(std::move(infra)) {}


    void Run () ;

    // Tick all tickable geometries
    void RunGeometryTick ();


    std::unique_ptr<MIInfraInterface> infra_;

    std::unique_ptr<std::thread> rhi_thread_;
    std::unique_ptr<std::thread> render_thread_;

};

std::unique_ptr<MainLoop> CreateFoundationsAndMainLoop (std::unique_ptr<MIInfraInterface> infra);

MI_NAMESPACE_END

#endif //MIRENDERER_ML_H
