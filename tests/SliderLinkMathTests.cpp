#include <SliderLinkMath.h>

#include <iostream>

namespace
{
    constexpr std::size_t ambient = 0;
    constexpr std::size_t directional = 1;
    constexpr std::size_t ambientColors = 2;
    constexpr std::size_t fogFar = 3;
    constexpr std::size_t fogNear = 4;
    using Values = std::array<double, 5>;
    struct Link
    {
        std::size_t index;
        double scale;
    };
    using Topology = std::array<std::optional<Link>, 5>;
    using Overrides = std::array<std::optional<Topology>, 5>;

    bool Expect(const bool a_condition, const char* a_message)
    {
        if (!a_condition) std::cerr << "FAILED: " << a_message << '\n';
        return a_condition;
    }

    bool Near(const double a_left, const double a_right)
    {
        return std::abs(a_left - a_right) < 0.000001;
    }

    double Unconstrained(const std::size_t, const double a_value) { return a_value; }
}

int main()
{
    namespace Math = MPL::SliderLinkMath;
    bool passed = true;
    const Values neutral{ 1.0, 1.0, 1.0, 1.0, 1.0 };
    const Topology profileLinks{ Link{ ambientColors, 1.0 }, Link{ ambientColors, 1.0 },
        std::nullopt, Link{ ambientColors, 1.0 }, Link{ ambientColors, 1.0 } };

    for (const auto ambientValue : std::array{ 0.5, 1.0, 2.0 })
    {
        for (const auto directionalValue : std::array{ 0.0, 0.5, 1.0, 1.1, 2.0 })
        {
            auto profile = neutral;
            profile[directional] = directionalValue;
            Overrides overrides;
            overrides[directional] = Topology{};
            const auto direct = Math::SeparateContributions(profile, overrides);
            passed &= Expect(profile == neutral, "the independent slider is removed from the profile contribution");
            profile[ambientColors] *= ambientValue;
            const auto gains = Math::ResolveBrightnessGains(profile, profileLinks, &direct, Unconstrained);
            passed &= Expect(Near(gains[directional], ambientValue * directionalValue),
                "Lux filtered ambient brightness stacks with the standard directional slider through its neutral value");
            for (const auto field : std::array{ ambient, ambientColors, fogFar, fogNear })
                passed &= Expect(Near(gains[field], ambientValue),
                    "the directional override cannot disconnect or modify the other profile links");
        }
    }

    auto profile = neutral;
    profile[ambientColors] = 2.0;
    profile[directional] = 0.5;
    Overrides cellOverride;
    cellOverride[directional] = Topology{};
    const auto cellDirect = Math::SeparateContributions(profile, cellOverride);
    const auto cellGains = Math::ResolveBrightnessGains(profile, profileLinks, &cellDirect, Unconstrained);
    passed &= Expect(Near(cellGains[directional], 1.0) && Near(cellGains[ambientColors], 2.0),
        "direct cell lighting stacks standard ambient and directional settings without a filtered rule");

    profile = neutral;
    profile[ambientColors] = 2.0;
    Overrides rootOverride;
    rootOverride[ambientColors] = Topology{};
    auto direct = Math::SeparateContributions(profile, rootOverride);
    auto gains = Math::ResolveBrightnessGains(profile, profileLinks, &direct, Unconstrained);
    passed &= Expect(Near(gains[ambientColors], 2.0) && Near(gains[directional], 1.0) && Near(gains[fogFar], 1.0),
        "an overridden root is applied once and does not propagate through profile links");

    profile = neutral;
    profile[directional] = 2.0;
    profile[fogNear] = 0.5;
    Overrides separateOverrides;
    Topology directionalLinks;
    directionalLinks[fogFar] = Link{ directional, 0.5 };
    separateOverrides[directional] = directionalLinks;
    separateOverrides[fogNear] = Topology{};
    direct = Math::SeparateContributions(profile, separateOverrides);
    profile[ambientColors] = 2.0;
    gains = Math::ResolveBrightnessGains(profile, profileLinks, &direct, Unconstrained);
    passed &= Expect(Near(gains[directional], 4.0) && Near(gains[fogFar], 3.0) && Near(gains[fogNear], 1.0),
        "two independent sliders retain their own custom links and stack on the profile gain");

    Values filteredValues = neutral;
    filteredValues[directional] = 0.5;
    std::array<bool, 5> filteredActive{};
    filteredActive[directional] = true;
    const auto filteredDirect = Math::ResolveContribution(filteredValues, filteredActive, Topology{});
    for (std::size_t field = 0; field < direct.size(); ++field) direct[field] *= filteredDirect[field];
    gains = Math::ResolveBrightnessGains(profile, profileLinks, &direct, Unconstrained);
    passed &= Expect(Near(gains[directional], 2.0) && Near(gains[fogFar], 3.0),
        "filtered and standard custom-link multipliers stack without leaking into each other's links");

    profile = neutral;
    profile[directional] = 0.5;
    Overrides directionalOverride;
    directionalOverride[directional] = Topology{};
    direct = Math::SeparateContributions(profile, directionalOverride);
    profile[ambientColors] = 2.0;
    gains = Math::ResolveBrightnessGains(profile, profileLinks, &direct,
        [](const std::size_t a_field, const double a_value)
        { return std::min(a_value, a_field == ambientColors ? 1.5 : 4.0); });
    passed &= Expect(Near(gains[directional], 0.75) && Near(gains[fogFar], 1.5),
        "directional multiplies the master's applied gain after its color ceiling is respected");

    profile = neutral;
    profile[ambientColors] = 2.0;
    direct = Math::SeparateContributions(profile, Overrides{});
    gains = Math::ResolveBrightnessGains(profile, profileLinks, &direct, Unconstrained);
    passed &= Expect(std::ranges::all_of(gains, [](const double a_gain) { return Near(a_gain, 2.0); }),
        "profiles without slider overrides keep their original links");
    return passed ? 0 : 1;
}
