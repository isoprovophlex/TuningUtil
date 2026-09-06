#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace MPL::HueMath
{
    inline constexpr double kPeriod = 255.0;
    inline constexpr double kTolerance = 0.0001;

    inline std::optional<double> Value(const double a_red, const double a_green, const double a_blue)
    {
        const double maximum = std::max({ a_red, a_green, a_blue });
        const double minimum = std::min({ a_red, a_green, a_blue });
        const double delta = maximum - minimum;
        if (delta <= kTolerance) return std::nullopt;

        double hue = 0.0;
        if (maximum == a_red)
            hue = 60.0 * std::fmod((a_green - a_blue) / delta, 6.0);
        else if (maximum == a_green)
            hue = 60.0 * (((a_blue - a_red) / delta) + 2.0);
        else
            hue = 60.0 * (((a_red - a_green) / delta) + 4.0);
        return (hue < 0.0 ? hue + 360.0 : hue) * (kPeriod / 360.0);
    }

    inline double Normalize(const double a_hue)
    {
        const double normalized = std::fmod(a_hue, kPeriod);
        return normalized < 0.0 ? normalized + kPeriod : normalized;
    }

    inline bool InRange(const double a_hue, const double a_start, const double a_end)
    {
        if (!std::isfinite(a_hue) || !std::isfinite(a_start) || !std::isfinite(a_end)) return false;
        if (std::abs(a_end - a_start) >= kPeriod - 0.001) return true;
        const double span = Normalize(a_end - a_start);
        return span > kTolerance && Normalize(a_hue - Normalize(a_start)) <= span;
    }
}  // namespace MPL::HueMath
