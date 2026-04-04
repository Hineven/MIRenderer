/*
 * Created: 2026/04/04
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "viewer_export_channel.h"

#include <array>

MI_NAMESPACE_BEGIN

namespace {

struct ViewerFrameExportChannelDesc {
    std::string_view name;
    ViewerFrameExportChannel channel;
};

constexpr std::array<ViewerFrameExportChannelDesc, 7> kViewerFrameExportChannels = {{
    {"radiance", ViewerFrameExportChannel::kRadiance},
    {"overlay", ViewerFrameExportChannel::kOverlay},
    {"depth", ViewerFrameExportChannel::kDepth},
    {"grf_depth", ViewerFrameExportChannel::kGrfDepth},
    {"grf_opacity", ViewerFrameExportChannel::kGrfOpacity},
    {"transmittance", ViewerFrameExportChannel::kTransmittance},
    {"visibility", ViewerFrameExportChannel::kVisibility},
}};

} // namespace

bool TryParseViewerFrameExportChannel(std::string_view name, ViewerFrameExportChannel& out_channel) {
    for (const auto& desc : kViewerFrameExportChannels) {
        if (desc.name == name) {
            out_channel = desc.channel;
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& GetSupportedViewerFrameExportChannelNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> result;
        result.reserve(kViewerFrameExportChannels.size());
        for (const auto& desc : kViewerFrameExportChannels) {
            result.emplace_back(desc.name);
        }
        return result;
    }();
    return names;
}

std::string GetSupportedViewerFrameExportChannelListString() {
    const auto& names = GetSupportedViewerFrameExportChannelNames();
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            joined += ", ";
        }
        joined += names[i];
    }
    return joined;
}

MI_NAMESPACE_END
