#pragma once

#include <algorithm>
#include <cstdint>

namespace MPL::PointLightFade
{
    template <class Rules, class MatchesSource>
    double Resolve(
        const double a_baseMultiplier, const std::uint32_t a_baseID,
        const Rules& a_rules, const MatchesSource& a_matchesSource)
    {
        auto multiplier = std::max(0.0, a_baseMultiplier);
        for (const auto& rule : a_rules)
            if (rule.baseLights.contains(a_baseID) && a_matchesSource(rule.xemiFilter))
                multiplier *= std::max(0.0, rule.fadeMultiplier);
        return multiplier;
    }
}
