#pragma once

#include <algorithm>
#include <optional>

namespace MPL::RulePriority
{
    template <class Rules, class MatchesRule, class Multiplier>
    double ResolveMultiplier(
        const double a_baseMultiplier,
        const Rules& a_rules,
        const MatchesRule& a_matchesRule,
        const Multiplier& a_multiplier)
    {
        auto result = std::max(0.0, a_baseMultiplier);
        auto group = a_rules.begin();
        while (group != a_rules.end())
        {
            auto groupEnd = group;
            while (groupEnd != a_rules.end() && groupEnd->profile == group->profile) ++groupEnd;

            std::optional<int> winningPriority;
            auto profileMultiplier = 1.0;
            for (auto rule = group; rule != groupEnd; ++rule)
            {
                const auto candidate = a_multiplier(*rule);
                if (!candidate || !a_matchesRule(*rule)) continue;
                if (!winningPriority || rule->priority > *winningPriority)
                {
                    winningPriority = rule->priority;
                    profileMultiplier = std::max(0.0, *candidate);
                }
                else if (rule->priority == *winningPriority)
                {
                    profileMultiplier *= std::max(0.0, *candidate);
                }
            }
            if (winningPriority) result *= profileMultiplier;
            group = groupEnd;
        }
        return result;
    }
}
