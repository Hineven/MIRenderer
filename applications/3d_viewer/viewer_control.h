#pragma once

#include <vector>
#include <string>
#include <imgui.h>
#include "viewer_app.h"

MI_NAMESPACE_BEGIN

namespace ViewerControlUI {
    void DrawControlUI(ViewerApp& app, ViewerApp::FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration);
}

MI_NAMESPACE_END

