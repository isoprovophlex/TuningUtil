#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace MPL::SliderLinkMath
{
    template <std::size_t N, class Topology>
    std::array<double, N> ResolveContribution(
        std::array<double, N> a_values,
        std::array<bool, N> a_active,
        const Topology& a_links)
    {
        std::array<std::uint8_t, N> states{};
        const auto resolve = [&](auto&& a_self, const std::size_t a_field) -> bool
        {
            if (states[a_field] == 2) return a_active[a_field];
            if (states[a_field] == 1) return false;
            states[a_field] = 1;
            if (a_links[a_field])
            {
                const auto source = a_links[a_field]->index;
                a_active[a_field] = a_self(a_self, source);
                if (a_active[a_field])
                    a_values[a_field] = 1.0 + ((a_values[source] - 1.0) * a_links[a_field]->scale);
            }
            states[a_field] = 2;
            return a_active[a_field];
        };
        std::array<double, N> result;
        result.fill(1.0);
        for (std::size_t field = 0; field < N; ++field)
            if (resolve(resolve, field)) result[field] = a_values[field];
        return result;
    }

    template <std::size_t N, class Topology>
    std::array<double, N> SeparateContributions(
        std::array<double, N>& a_profileValues,
        const std::array<std::optional<Topology>, N>& a_overrides)
    {
        std::array<double, N> result;
        result.fill(1.0);
        for (std::size_t source = 0; source < N; ++source)
        {
            if (!a_overrides[source]) continue;
            std::array<double, N> values;
            values.fill(1.0);
            values[source] = a_profileValues[source];
            a_profileValues[source] = 1.0;
            std::array<bool, N> active{};
            active[source] = true;
            const auto contribution = ResolveContribution(values, active, *a_overrides[source]);
            for (std::size_t field = 0; field < N; ++field) result[field] *= contribution[field];
        }
        return result;
    }

    template <std::size_t N, class Topology, class Constrain>
    std::array<double, N> ResolveBrightnessGains(
        const std::array<double, N>& a_values,
        const Topology& a_links,
        const std::array<double, N>* a_directMultipliers,
        Constrain&& a_constrain)
    {
        std::array<double, N> gains{};
        std::array<bool, N> resolved{};
        const auto resolve = [&](auto&& a_self, const std::size_t a_field) -> double
        {
            if (resolved[a_field]) return gains[a_field];
            if (const auto link = a_links[a_field])
                gains[a_field] = std::max(0.0, 1.0 + ((a_self(a_self, link->index) - 1.0) * link->scale));
            else
                gains[a_field] = a_constrain(a_field, a_values[a_field]);
            resolved[a_field] = true;
            return gains[a_field];
        };
        for (std::size_t field = 0; field < N; ++field) resolve(resolve, field);
        if (a_directMultipliers)
        {
            for (std::size_t field = 0; field < N; ++field)
            {
                const auto multiplier = (*a_directMultipliers)[field];
                if (std::abs(multiplier - 1.0) > 0.0001)
                    gains[field] = a_constrain(field, gains[field] * multiplier);
            }
        }
        return gains;
    }
}
