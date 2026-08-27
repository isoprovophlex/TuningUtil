#pragma once

#include <optional>
#include <string_view>

namespace MPL::ImageSpacePatcher
{
    struct RuntimeMonitor
    {
        float filmicWhiteScale = 0.0f;
        bool filmicCurve = false;
        bool filmicCurveAvailable = false;
        bool filmicWhiteScaleAvailable = false;
    };

    void ApplyFilmicCurveWhitePoint();
    std::optional<bool> IsAutoCSTonemappingApplied(std::string_view);
    void RequestRuntimeMonitorRefresh();
    RuntimeMonitor ReadRuntimeMonitor();
    void ReleaseRuntimeState();
}
