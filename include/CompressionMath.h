#pragma once

#include <algorithm>
#include <cmath>

namespace MPL::CompressionMath
{
    inline double BetweenCompressionValue(
        const double a_value,
        const double a_compression,
        const double a_anchor)
    {
        if (a_value <= 0.0001 || a_anchor <= 0.0001)
        {
            return a_value;
        }
        const auto compression = std::clamp(a_compression, -2.0, 1.0);
        if (std::abs(compression) <= 0.0001 || (compression > 0.0 && a_value >= a_anchor))
        {
            return a_value;
        }
        return std::exp(
            (1.0 - compression) * std::log(a_value) +
            compression * std::log(a_anchor));
    }

    inline double BetweenCompressionGain(
        const double a_value,
        const double a_anchor,
        const double a_requestedGain)
    {
        if (a_value >= a_anchor && a_requestedGain < 1.0)
        {
            return 1.0;
        }
        return a_requestedGain;
    }
}  // namespace MPL::CompressionMath
