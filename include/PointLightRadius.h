#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace MPL::PointLightRadius
{
    inline double Scale(const double a_baseRadius, const double a_referenceOffset, const double a_multiplier)
    {
        const auto radius = std::max(0.0, a_baseRadius + a_referenceOffset);
        if (radius == 0.0) return 0.0;
        return std::clamp(radius * std::max(0.0, a_multiplier),
            0.0, static_cast<double>(std::numeric_limits<std::int32_t>::max()));
    }

    inline std::uint32_t Record(const double a_baseRadius, const double a_multiplier)
    {
        return static_cast<std::uint32_t>(std::round(Scale(a_baseRadius, 0.0, a_multiplier)));
    }

    inline double LightPlacer(const double a_configuredRadius, const double a_baseRadius, const double a_multiplier)
    {
        // Light Placer treats zero as a request to fall back to the base record.
        constexpr double minimumRadius = 0.01;
        return std::max(minimumRadius, Scale(a_configuredRadius > 0.0 ? a_configuredRadius : a_baseRadius, 0.0, a_multiplier));
    }
}
