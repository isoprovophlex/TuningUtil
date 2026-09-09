#pragma once

#include <RulePriority.h>
#include <cstdint>

namespace MPL::PointLightFade
{
    template <class Rules, class MatchesSource, class Multiplier>
    double ResolveMultiplier(
        const double a_baseMultiplier, const std::uint32_t a_baseID,
        const Rules& a_rules, const MatchesSource& a_matchesSource, const Multiplier& a_multiplier)
    {
        return RulePriority::ResolveMultiplier(a_baseMultiplier, a_rules,
            [&](const auto& a_rule)
            {
                return a_rule.baseLights.contains(a_baseID) &&
                       (!a_rule.useXemiFilter || a_matchesSource(a_rule.xemiFilter));
            }, a_multiplier);
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
