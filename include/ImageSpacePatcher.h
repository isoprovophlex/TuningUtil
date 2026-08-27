#pragma once

namespace MPL::ImageSpacePatcher
{
    struct RuntimeMonitor
    {
        float whitePoint = 0.0f;
        float filmicWhiteScale = 0.0f;
        bool filmicCurve = false;
        bool whitePointAvailable = false;
        bool filmicCurveAvailable = false;
        bool filmicWhiteScaleAvailable = false;
    };

    void ApplyFilmicCurveWhitePoint();
    void RequestRuntimeMonitorRefresh();
    RuntimeMonitor ReadRuntimeMonitor();
    void ReleaseRuntimeState();
}
