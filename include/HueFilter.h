#pragma once

#include <HueMath.h>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::HueFilter
{
    inline constexpr std::array<std::string_view, 7> kBands{
        "red", "orange", "yellow", "green", "teal", "blue", "magenta"
    };

    struct Selection
    {
        std::vector<std::string> include;
        bool Empty() const { return include.empty(); }
        bool operator==(const Selection&) const = default;
    };

    inline bool Valid(const Selection& a_filter)
    {
        const auto validBand = [](const auto& a_band)
        { return std::find(kBands.begin(), kBands.end(), a_band) != kBands.end(); };
        return std::all_of(a_filter.include.begin(), a_filter.include.end(), validBand);
    }

    template <class Ranges>
    bool Matches(const std::optional<double> a_hue, const Ranges& a_ranges, const Selection& a_filter)
    {
        if (a_filter.Empty()) return true;
        const std::array ranges{
            &a_ranges.red, &a_ranges.orange, &a_ranges.yellow, &a_ranges.green,
            &a_ranges.teal, &a_ranges.blue, &a_ranges.magenta
        };
        const auto matchesAny = [&](const auto& a_bands)
        {
            if (!a_hue) return false;
            for (const auto& band : a_bands)
            {
                const auto found = std::find(kBands.begin(), kBands.end(), band);
                if (found == kBands.end()) continue;
                const auto* range = ranges[static_cast<std::size_t>(found - kBands.begin())];
                if (HueMath::InRange(*a_hue, range->start, range->end)) return true;
            }
            return false;
        };
        return matchesAny(a_filter.include);
    }
}  // namespace MPL::HueFilter
