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
    std::string window_name;
};

class MainLoop {
public:
    // Start the main loop, handle control to the renderer.
    static void Start (std::unique_ptr<MIInfraInterface> && infra, MainLoopStartConfig cfg) ;
    static MainLoop & Get ();

protected:

    void StartWindow ();

    void Run () ;

    MainLoopStartConfig config_;
    std::unique_ptr<std::thread> render_thread_;
};

MI_NAMESPACE_END

#endif //MIRENDERER_ML_H
