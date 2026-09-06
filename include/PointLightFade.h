#pragma once

#include <algorithm>
#include <cstdint>

namespace MPL::PointLightFade
{
    template <class Rules, class MatchesSource, class Multiplier>
    double ResolveMultiplier(
        const double a_baseMultiplier, const std::uint32_t a_baseID,
        const Rules& a_rules, const MatchesSource& a_matchesSource, const Multiplier& a_multiplier)
    {
        auto multiplier = std::max(0.0, a_baseMultiplier);
        for (const auto& rule : a_rules)
            if (rule.baseLights.contains(a_baseID) && a_matchesSource(rule.xemiFilter))
                multiplier *= std::max(0.0, a_multiplier(rule));
        return multiplier;
    }

    template <class Rules, class MatchesSource>
    double Resolve(
        const double a_baseMultiplier, const std::uint32_t a_baseID,
        const Rules& a_rules, const MatchesSource& a_matchesSource)
    {
        return ResolveMultiplier(a_baseMultiplier, a_baseID, a_rules, a_matchesSource,
            [](const auto& rule) { return rule.fadeMultiplier; });
    }

    template <class Rules, class MatchesSource>
    double ResolveRadius(
        const double a_baseMultiplier, const std::uint32_t a_baseID,
        const Rules& a_rules, const MatchesSource& a_matchesSource)
    {
        return ResolveMultiplier(a_baseMultiplier, a_baseID, a_rules, a_matchesSource,
            [](const auto& rule) { return rule.radiusMultiplier; });
    }
}
