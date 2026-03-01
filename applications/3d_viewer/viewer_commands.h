#pragma once

#include <string>
#include "viewer_app.h"

MI_NAMESPACE_BEGIN
class ViewerApp;

// Register viewer-specific console commands.
void RegisterViewerCommands(ViewerApp& app);

MI_NAMESPACE_END
