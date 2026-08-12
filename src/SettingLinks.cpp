#include <SettingLinks.h>
#include <algorithm>
#include <array>
#include <functional>

namespace MPL::WeatherPatcher
{
    namespace
    {
        struct LinkField
        {
            std::string_view key;
            const SettingLink* value;
        };

        enum class LinkState : std::uint8_t
        {
            kUnvisited,
            kVisiting,
            kResolved,
            kFailed,
        };

        template <std::size_t N>
        std::array<std::optional<SettingLinkResolution>, N> ResolveLinks(
            const std::string_view a_category,
            const std::array<LinkField, N>& a_fields)
        {
            std::array<std::optional<SettingLinkResolution>, N> links{};
            std::array<LinkState, N> states{};
            std::function<bool(std::size_t)> resolve = [&](const std::size_t a_index)
            {
                if (states[a_index] == LinkState::kResolved) return true;
                if (states[a_index] == LinkState::kFailed) return false;
                if (states[a_index] == LinkState::kVisiting)
                {
                    logger::warn("[TuningUtil] shared link | category={} | field={} | status=circular", a_category, a_fields[a_index].key);
                    states[a_index] = LinkState::kFailed;
                    return false;
                }

                states[a_index] = LinkState::kVisiting;
                if (!*a_fields[a_index].value)
                {
                    states[a_index] = LinkState::kResolved;
                    return true;
                }

                const auto& [targetKey, scale] = **a_fields[a_index].value;

                const auto target = std::ranges::find(a_fields, targetKey, &LinkField::key);
                if (target == a_fields.end())
                {
                    logger::warn(
                        "[TuningUtil] shared link | {}.{} -> {} | status=invalid",
                        a_category,
                        a_fields[a_index].key,
                        targetKey);
                    states[a_index] = LinkState::kFailed;
                    return false;
                }

                const auto targetIndex = static_cast<std::size_t>(std::distance(a_fields.begin(), target));
                links[a_index] = SettingLinkResolution{ targetIndex, scale };
                if (!resolve(targetIndex))
                {
                    links[a_index].reset();
                    states[a_index] = LinkState::kFailed;
                    return false;
                }
                states[a_index] = LinkState::kResolved;
                return true;
            };

            for (std::size_t index = 0; index < N; ++index) resolve(index);
            return links;
        }

        template <std::size_t N>
        std::array<double, N> ResolveLinkedValues(
            std::array<double, N> a_values,
            const std::array<std::optional<SettingLinkResolution>, N>& a_links,
            const double a_neutral)
        {
            std::array<bool, N> resolved{};
            std::function<double(std::size_t)> resolve = [&](const std::size_t a_index)
            {
                if (!resolved[a_index])
                {
                    if (const auto link = a_links[a_index])
                    {
                        a_values[a_index] = a_neutral + ((resolve(link->index) - a_neutral) * link->scale);
                    }
                    resolved[a_index] = true;
                }
                return a_values[a_index];
            };
            for (std::size_t index = 0; index < N; ++index) resolve(index);
            return a_values;
        }

        template <std::size_t N>
        std::array<AmbientHueScaleValues, N> ResolveLinkedHueShift(
            std::array<AmbientHueScaleValues, N> a_values,
            const std::array<std::optional<SettingLinkResolution>, N>& a_links)
        {
            std::array<bool, N> resolved{};
            std::function<AmbientHueScaleValues(std::size_t)> resolve = [&](const std::size_t a_index)
            {
                if (!resolved[a_index])
                {
                    if (const auto link = a_links[a_index])
                    {
                        const auto source = resolve(link->index);
                        a_values[a_index] = {
                            source.red * link->scale, source.orange * link->scale, source.yellow * link->scale,
                            source.green * link->scale, source.teal * link->scale, source.blue * link->scale,
                            source.magenta * link->scale
                        };
                    }
                    resolved[a_index] = true;
                }
                return a_values[a_index];
            };
            for (std::size_t index = 0; index < N; ++index) resolve(index);
            return a_values;
        }

        template <class Settings>
        auto WeatherFields(const Settings& a_settings)
        {
            return std::array{
                LinkField{ "ambient", &a_settings.ambient }, LinkField{ "sunlight", &a_settings.sunlight },
                LinkField{ "effectLighting", &a_settings.effectLighting }, LinkField{ "fogFar", &a_settings.fogFar },
                LinkField{ "fogNear", &a_settings.fogNear }, LinkField{ "water", &a_settings.water },
                LinkField{ "skyStatics", &a_settings.skyStatics }, LinkField{ "skyUpper", &a_settings.skyUpper },
                LinkField{ "skyLower", &a_settings.skyLower }, LinkField{ "horizon", &a_settings.horizon },
                LinkField{ "sun", &a_settings.sun }, LinkField{ "sunGlare", &a_settings.sunGlare },
                LinkField{ "moonGlare", &a_settings.moonGlare }, LinkField{ "stars", &a_settings.stars }
            };
        }

        auto WeatherColorFields(const WeatherLinks& a_settings)
        {
            return std::array{
                LinkField{ "ambient", &a_settings.ambient }, LinkField{ "sunlight", &a_settings.sunlight },
                LinkField{ "effectLighting", &a_settings.effectLighting }, LinkField{ "fogFar", &a_settings.fogFar },
                LinkField{ "fogNear", &a_settings.fogNear }, LinkField{ "water", &a_settings.water },
                LinkField{ "skyStatics", &a_settings.skyStatics }, LinkField{ "skyUpper", &a_settings.skyUpper },
                LinkField{ "skyLower", &a_settings.skyLower }, LinkField{ "horizon", &a_settings.horizon },
                LinkField{ "sun", &a_settings.sun }, LinkField{ "sunGlare", &a_settings.sunGlare },
                LinkField{ "moonGlare", &a_settings.moonGlare }, LinkField{ "stars", &a_settings.stars },
                LinkField{ "cloudLayers", &a_settings.cloudLayers }
            };
        }

        std::array<AmbientHueScaleValues, 16> HueShiftValues(const HueShiftSettings& a_settings)
        {
            const auto convert = [](const HueShiftBands& a_value)
            {
                return AmbientHueScaleValues{
                    a_value.red, a_value.orange, a_value.yellow, a_value.green,
                    a_value.teal, a_value.blue, a_value.magenta
                };
            };
            return {
                convert(a_settings.ambient), convert(a_settings.sunlight), convert(a_settings.effectLighting),
                convert(a_settings.fogFar), convert(a_settings.fogNear), convert(a_settings.water),
                convert(a_settings.skyStatics), convert(a_settings.skyUpper), convert(a_settings.skyLower),
                convert(a_settings.horizon), convert(a_settings.sun), convert(a_settings.sunGlare),
                convert(a_settings.moonGlare), convert(a_settings.stars), convert(a_settings.cloudLayers),
                convert(a_settings.volumetricLighting)
            };
        }
    }

    BrightnessResolution ResolveBrightnessWithLinks(const BrightnessSettings& a_settings, const WeatherLinks& a_links)
    {
        const auto links = ResolveLinks("links.weather", WeatherColorFields(a_links));
        return {
            .values = {
                a_settings.ambient, a_settings.sunlight, a_settings.effectLighting, a_settings.fogFar,
                a_settings.fogNear, a_settings.water, a_settings.skyStatics, a_settings.skyUpper,
                a_settings.skyLower, a_settings.horizon, a_settings.sun, a_settings.sunGlare,
                a_settings.moonGlare, a_settings.stars, a_settings.cloudLayers },
            .links = links,
        };
    }

    CompressionResolution ResolveCompressionWithLinks(const CompressionSettings& a_settings, const WeatherLinks& a_links)
    {
        return {
            .values = {
                a_settings.ambient, a_settings.sunlight, a_settings.effectLighting, a_settings.fogFar,
                a_settings.fogNear, a_settings.water, a_settings.skyStatics, a_settings.skyUpper,
                a_settings.skyLower, a_settings.horizon, a_settings.sun, a_settings.sunGlare,
                a_settings.moonGlare, a_settings.stars },
            .links = ResolveLinks("links.weather", WeatherFields(a_links)),
        };
    }

    CompressionResolution ResolveWithinWeatherCompressionWithLinks(
        const CompressionSettings& a_settings,
        const WeatherLinks& a_links)
    {
        return ResolveCompressionWithLinks(a_settings, a_links);
    }

    SaturationResolution ResolveSaturation(const SaturationSettings& a_settings, const WeatherLinks& a_links)
    {
        const auto fields = std::array{
            LinkField{ "ambient", &a_links.ambient }, LinkField{ "sunlight", &a_links.sunlight },
            LinkField{ "effectLighting", &a_links.effectLighting }, LinkField{ "fogFar", &a_links.fogFar },
            LinkField{ "fogNear", &a_links.fogNear }, LinkField{ "water", &a_links.water },
            LinkField{ "skyStatics", &a_links.skyStatics }, LinkField{ "skyUpper", &a_links.skyUpper },
            LinkField{ "skyLower", &a_links.skyLower }, LinkField{ "horizon", &a_links.horizon },
            LinkField{ "sun", &a_links.sun }, LinkField{ "sunGlare", &a_links.sunGlare },
            LinkField{ "moonGlare", &a_links.moonGlare }, LinkField{ "stars", &a_links.stars },
            LinkField{ "cloudLayers", &a_links.cloudLayers }, LinkField{ "volumetricLighting", &a_links.volumetricLighting }
        };
        const auto links = ResolveLinks("links.weather", fields);
        const auto values = ResolveLinkedValues(std::array{
            a_settings.ambient, a_settings.sunlight, a_settings.effectLighting, a_settings.fogFar,
            a_settings.fogNear, a_settings.water, a_settings.skyStatics, a_settings.skyUpper,
            a_settings.skyLower, a_settings.horizon, a_settings.sun, a_settings.sunGlare,
            a_settings.moonGlare, a_settings.stars, a_settings.cloudLayers, a_settings.volumetricLighting
        }, links, 0.0);
        return {
            .values = {
                values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                values[8], values[9], values[10], values[11], values[12], values[13], values[14], values[15] },
            .links = links,
        };
    }

    AmbientHueScaleValues ResolveHueScales(const AmbientHueScales& a_settings)
    {
        return { a_settings.red, a_settings.orange, a_settings.yellow, a_settings.green,
            a_settings.teal, a_settings.blue, a_settings.magenta };
    }

    HueShiftResolution ResolveHueShift(const HueShiftSettings& a_settings, const WeatherLinks& a_links)
    {
        const auto fields = std::array{
            LinkField{ "ambient", &a_links.ambient }, LinkField{ "sunlight", &a_links.sunlight },
            LinkField{ "effectLighting", &a_links.effectLighting }, LinkField{ "fogFar", &a_links.fogFar },
            LinkField{ "fogNear", &a_links.fogNear }, LinkField{ "water", &a_links.water },
            LinkField{ "skyStatics", &a_links.skyStatics }, LinkField{ "skyUpper", &a_links.skyUpper },
            LinkField{ "skyLower", &a_links.skyLower }, LinkField{ "horizon", &a_links.horizon },
            LinkField{ "sun", &a_links.sun }, LinkField{ "sunGlare", &a_links.sunGlare },
            LinkField{ "moonGlare", &a_links.moonGlare }, LinkField{ "stars", &a_links.stars },
            LinkField{ "cloudLayers", &a_links.cloudLayers }, LinkField{ "volumetricLighting", &a_links.volumetricLighting }
        };
        const auto links = ResolveLinks("links.weather", fields);
        return { .values = ResolveLinkedHueShift(HueShiftValues(a_settings), links), .links = links };
    }

    AnchorValues ResolveAnchors(const CompressionAnchorSettings& a_settings, const WeatherLinks& a_links)
    {
        const auto links = ResolveLinks("links.weather", WeatherFields(a_links));
        const auto values = ResolveLinkedValues(std::array{
            a_settings.ambient, a_settings.sunlight, a_settings.effectLighting, a_settings.fogFar,
            a_settings.fogNear, a_settings.water, a_settings.skyStatics, a_settings.skyUpper,
            a_settings.skyLower, a_settings.horizon, a_settings.sun, a_settings.sunGlare,
            a_settings.moonGlare, a_settings.stars
        }, links, 0.0);
        return { values[0], values[1], values[2], values[3], values[4], values[5], values[6],
            values[7], values[8], values[9], values[10], values[11], values[12], values[13] };
    }
}  // namespace MPL::WeatherPatcher

namespace MPL::LightingPatcher
{
    InteriorLinkTopology ResolveInteriorLinks(const InteriorLinks& a_links)
    {
        using WeatherPatcher::LinkField;
        using WeatherPatcher::ResolveLinks;
        return ResolveLinks("links.interior", std::array{
            LinkField{ "ambient", &a_links.ambient },
            LinkField{ "directional", &a_links.directional },
            LinkField{ "ambientColors", &a_links.ambientColors },
            LinkField{ "fogFar", &a_links.fogFar },
            LinkField{ "fogNear", &a_links.fogNear },
        });
    }
}  // namespace MPL::LightingPatcher
