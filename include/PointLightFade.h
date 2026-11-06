#pragma once

#include <RulePriority.h>
#include <algorithm>
#include <cstdint>

namespace MPL::PointLightFade
{
    inline float ReferenceOffset(
        const float a_sourceBase, const float a_sourceOffset,
        const double a_baseMultiplier, const double a_referenceMultiplier)
    {
        const auto intensity = std::max(0.0, static_cast<double>(a_sourceBase) + a_sourceOffset);
        return static_cast<float>(intensity * std::max(0.0, a_referenceMultiplier) -
                                  a_sourceBase * std::max(0.0, a_baseMultiplier));
    }

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
