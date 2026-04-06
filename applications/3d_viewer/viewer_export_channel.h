/*
 * Created: 2026/04/04
 * Author:  hineven
 * See LICENSE for licensing.
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "fwd.h"

MI_NAMESPACE_BEGIN

enum class ViewerFrameExportChannel {
    kRadiance,
    kColor,
    kOverlay,
    kDepth,
    kGrfDepth,
    kGrfOpacity,
    kTransmittance,
    kVisibility,
    kNormal,
    kGeometryNormal,
    kAlbedo,
    kDiffuseDirect,
    kDiffuseIndirect,
    kDenoisedDiffuseDirect,
    kDenoisedDiffuseIndirect,
    kMotionVector,
    kPathTracing,
    kTonemappedPathTracing,
};

bool TryParseViewerFrameExportChannel(std::string_view name, ViewerFrameExportChannel& out_channel);
const std::vector<std::string>& GetSupportedViewerFrameExportChannelNames();
std::string GetSupportedViewerFrameExportChannelListString();

MI_NAMESPACE_END
