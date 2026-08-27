#include <Config.h>
#include <DetailedLogging.h>
#include <LightingPatcher.h>
#include <PointLightPatcher.h>
#include <RecordFilter.h>
#include <SettingLinks.h>
#include <WeatherPatcher.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>

namespace MPL::LightingPatcher
{
    namespace
    {
        std::atomic_bool retainRuntimeState{ true };
        constexpr std::size_t kFieldCount = 5;
        constexpr std::array<std::string_view, kFieldCount> kFieldNames{
            "ambient", "directional", "ambientColors", "fogFar", "fogNear"
        };

        struct Resolution
        {
            std::array<double, kFieldCount> values{};
            InteriorLinkTopology links{};
        };

        struct InteriorHueShiftResolution
        {
            std::array<WeatherPatcher::HueShiftBands, kFieldCount> values{};
        };

        Resolution ResolveCategory(
            const InteriorColorSettings& a_settings,
            const InteriorLinkTopology& a_links)
        {
            Resolution result{
                .values = { a_settings.ambient, a_settings.directional, a_settings.ambientColors,
                    a_settings.fogFar, a_settings.fogNear },
                .links = a_links,
            };
            std::array<bool, kFieldCount> resolved{};
            std::function<double(std::size_t)> resolve = [&](const std::size_t a_index)
            {
                if (!resolved[a_index])
                {
                    if (const auto link = result.links[a_index])
                        result.values[a_index] = resolve(link->index) * link->scale;
                    resolved[a_index] = true;
                }
                return result.values[a_index];
            };
            for (std::size_t index = 0; index < kFieldCount; ++index) resolve(index);
            return result;
        }

        InteriorHueShiftResolution ResolveHueShiftCategory(
            const InteriorHueShiftSettings& a_settings,
            const InteriorLinkTopology& a_links)
        {
            InteriorHueShiftResolution result{ .values = {
                a_settings.ambient,
                a_settings.directional,
                a_settings.ambientColors,
                a_settings.fogFar,
                a_settings.fogNear,
            } };
            std::array<bool, kFieldCount> resolved{};
            const auto scaleBands = [](WeatherPatcher::HueShiftBands a_bands, const double a_scale)
            {
                a_bands.red *= a_scale;
                a_bands.orange *= a_scale;
                a_bands.yellow *= a_scale;
                a_bands.green *= a_scale;
                a_bands.teal *= a_scale;
                a_bands.blue *= a_scale;
                a_bands.magenta *= a_scale;
                return a_bands;
            };

            std::function<WeatherPatcher::HueShiftBands(std::size_t)> resolve =
                [&](const std::size_t a_index)
            {
                if (!resolved[a_index])
                {
                    if (const auto link = a_links[a_index])
                        result.values[a_index] = scaleBands(resolve(link->index), link->scale);
                    resolved[a_index] = true;
                }
                return result.values[a_index];
            };
            for (std::size_t index = 0; index < kFieldCount; ++index) resolve(index);
            return result;
        }

        template <class Fn>
        void ForEachDALCColor(RE::BGSDirectionalAmbientLightingColors& a_colors, Fn&& a_fn)
        {
            a_fn(a_colors.directional.x.max);
            a_fn(a_colors.directional.x.min);
            a_fn(a_colors.directional.y.max);
            a_fn(a_colors.directional.y.min);
            a_fn(a_colors.directional.z.max);
            a_fn(a_colors.directional.z.min);
        }

        template <class Fn>
        void ForEachFieldColor(
            RE::INTERIOR_DATA& a_data,
            RE::BGSDirectionalAmbientLightingColors& a_ambientColors,
            const std::size_t a_field,
            Fn&& a_fn)
        {
            switch (a_field)
            {
            case 0:
                a_fn(a_data.ambient);
                break;
            case 1:
                a_fn(a_data.directional);
                break;
            case 2:
                ForEachDALCColor(a_ambientColors, a_fn);
                break;
            case 3:
                a_fn(a_data.fogColorFar);
                break;
            case 4:
                a_fn(a_data.fogColorNear);
                break;
            default:
                break;
            }
        }

        bool IsBlack(const RE::Color& a_color)
        {
            return a_color.red == 0 && a_color.green == 0 && a_color.blue == 0;
        }

        std::uint8_t ClampByte(const double a_value)
        {
            return static_cast<std::uint8_t>(std::clamp(std::round(a_value), 0.0, 255.0));
        }

        void MultiplyColor(RE::Color& a_color, double a_multiplier)
        {
            if (IsBlack(a_color))
            {
                return;
            }

            double multiplier = std::max(0.0, a_multiplier);
            if (multiplier > 1.0)
            {
                const double maxChannel = std::max({
                    static_cast<double>(a_color.red),
                    static_cast<double>(a_color.green),
                    static_cast<double>(a_color.blue),
                });
                multiplier = std::min(multiplier, 255.0 / maxChannel);
            }
            else if (multiplier < 1.0)
            {
                double minPositiveChannel = 255.0;
                for (const auto channel : { a_color.red, a_color.green, a_color.blue })
                {
                    if (channel > 0)
                    {
                        minPositiveChannel = std::min(minPositiveChannel, static_cast<double>(channel));
                    }
                }
                multiplier = std::max(multiplier, 1.0 / minPositiveChannel);
            }

            a_color.red = ClampByte(a_color.red * multiplier);
            a_color.green = ClampByte(a_color.green * multiplier);
            a_color.blue = ClampByte(a_color.blue * multiplier);
        }

        void SaturateColor(RE::Color& a_color, const double a_multiplier)
        {
            if (IsBlack(a_color))
            {
                return;
            }

            const double factor = std::max(0.0, a_multiplier);
            const double luminance = 0.299 * a_color.red + 0.587 * a_color.green + 0.114 * a_color.blue;
            a_color.red = ClampByte(luminance + ((a_color.red - luminance) * factor));
            a_color.green = ClampByte(luminance + ((a_color.green - luminance) * factor));
            a_color.blue = ClampByte(luminance + ((a_color.blue - luminance) * factor));
        }

        std::optional<double> HueRangeValue(const RE::Color& a_color)
        {
            const double red = a_color.red / 255.0;
            const double green = a_color.green / 255.0;
            const double blue = a_color.blue / 255.0;
            const double maximum = std::max({ red, green, blue });
            const double minimum = std::min({ red, green, blue });
            const double delta = maximum - minimum;
            if (delta <= 0.0001)
            {
                return std::nullopt;
            }

            double hue = 0.0;
            if (maximum == red)
            {
                hue = 60.0 * std::fmod((green - blue) / delta, 6.0);
            }
            else if (maximum == green)
            {
                hue = 60.0 * (((blue - red) / delta) + 2.0);
            }
            else
            {
                hue = 60.0 * (((red - green) / delta) + 4.0);
            }
            const auto degrees = hue < 0.0 ? hue + 360.0 : hue;
            return degrees * (255.0 / 360.0);
        }

        double HueScale(
            const RE::Color& a_color,
            const WeatherPatcher::AmbientHueScaleValues& a_scales,
            const WeatherPatcher::HueRanges& a_ranges)
        {
            const auto hue = HueRangeValue(a_color);
            if (!hue)
            {
                return 1.0;
            }

            const std::array<double, 7> scales{
                a_scales.red,
                a_scales.orange,
                a_scales.yellow,
                a_scales.green,
                a_scales.teal,
                a_scales.blue,
                a_scales.magenta,
            };
            const std::array<const WeatherPatcher::HueRange*, 7> ranges{
                &a_ranges.red,
                &a_ranges.orange,
                &a_ranges.yellow,
                &a_ranges.green,
                &a_ranges.teal,
                &a_ranges.blue,
                &a_ranges.magenta,
            };
            const auto normalize = [](const double a_value)
            {
                const double normalized = std::fmod(a_value, 255.0);
                return normalized < 0.0 ? normalized + 255.0 : normalized;
            };
            double totalWeight = 0.0;
            double scale = 0.0;
            for (std::size_t i = 0; i < ranges.size(); ++i)
            {
                if (std::abs(ranges[i]->end - ranges[i]->start) >= 254.999)
                {
                    totalWeight += 1.0;
                    scale += scales[i];
                    continue;
                }
                const double start = normalize(ranges[i]->start);
                const double span = normalize(ranges[i]->end - ranges[i]->start);
                if (span <= 0.0001)
                {
                    continue;
                }
                const double offset = normalize(*hue - start);
                if (offset > span)
                {
                    continue;
                }
                const double halfSpan = span * 0.5;
                const double weight = std::max(0.0, 1.0 - std::abs(offset - halfSpan) / halfSpan);
                totalWeight += weight;
                scale += scales[i] * weight;
            }
            return totalWeight > 0.0001 ? std::max(0.0, scale / totalWeight) : 1.0;
        }

        std::size_t LinkRoot(std::size_t a_field, const Resolution& a_resolution)
        {
            std::array<bool, kFieldCount> visited{};
            while (a_resolution.links[a_field] && !visited[a_field])
            {
                visited[a_field] = true;
                a_field = a_resolution.links[a_field]->index;
            }
            return a_field;
        }

        bool UsesAmbientHueScales(const std::size_t a_field, const Resolution& a_resolution)
        {
            const auto root = LinkRoot(a_field, a_resolution);
            return root == 0 || root == 2;
        }

        double ConstrainFieldGain(
            RE::INTERIOR_DATA& a_data,
            RE::BGSDirectionalAmbientLightingColors& a_ambientColors,
            const std::size_t a_field,
            const double a_requestedGain)
        {
            const double requestedGain = std::max(0.0, a_requestedGain);
            double maxChannel = 0.0;
            double minPositiveChannel = 256.0;
            ForEachFieldColor(a_data, a_ambientColors, a_field, [&](const RE::Color& a_color)
                {
                    for (const auto channel : { a_color.red, a_color.green, a_color.blue })
                    {
                        maxChannel = std::max(maxChannel, static_cast<double>(channel));
                        if (channel > 0)
                        {
                            minPositiveChannel = std::min(minPositiveChannel, static_cast<double>(channel));
                        }
                    } });

            if (maxChannel <= 0.0 || minPositiveChannel > 255.0)
            {
                return 1.0;
            }
            if (requestedGain > 1.0)
            {
                return std::min(requestedGain, 255.0 / maxChannel);
            }
            return std::max(requestedGain, 1.0 / minPositiveChannel);
        }

        void ApplyBrightness(
            RE::INTERIOR_DATA& a_data,
            RE::BGSDirectionalAmbientLightingColors& a_ambientColors,
            const Resolution& a_settings,
            const std::array<bool, kFieldCount>& a_active)
        {
            std::array<double, kFieldCount> gains{};
            std::array<bool, kFieldCount> resolved{};
            std::function<double(std::size_t)> resolveGain = [&](const std::size_t a_field)
            {
                if (resolved[a_field])
                {
                    return gains[a_field];
                }
                if (const auto link = a_settings.links[a_field])
                {
                    gains[a_field] = std::max(0.0, 1.0 + ((resolveGain(link->index) - 1.0) * link->scale));
                }
                else
                {
                    gains[a_field] = ConstrainFieldGain(a_data, a_ambientColors, a_field, a_settings.values[a_field]);
                }
                resolved[a_field] = true;
                return gains[a_field];
            };

            for (std::size_t field = 0; field < kFieldCount; ++field)
            {
                resolveGain(field);
            }
            for (std::size_t field = 0; field < kFieldCount; ++field)
            {
                if (!a_active[field] || std::abs(gains[field] - 1.0) <= 0.0001)
                {
                    continue;
                }
                ForEachFieldColor(a_data, a_ambientColors, field, [&](RE::Color& a_color)
                    { MultiplyColor(a_color, gains[field]); });
            }
        }

        void ApplyFogMax(RE::INTERIOR_DATA& a_data, const double a_multiplier, const bool a_active)
        {
            const auto multiplier = std::max(0.0, a_multiplier);
            if (!a_active || std::abs(multiplier - 1.0) <= 0.0001)
            {
                return;
            }
            const auto value = static_cast<double>(a_data.fogClamp) * multiplier;
            a_data.fogClamp = static_cast<float>(std::clamp(
                value,
                -static_cast<double>(std::numeric_limits<float>::max()),
                static_cast<double>(std::numeric_limits<float>::max())));
        }

        void ApplySaturation(
            RE::INTERIOR_DATA& a_data,
            RE::BGSDirectionalAmbientLightingColors& a_ambientColors,
            const Resolution& a_settings,
            const WeatherPatcher::AmbientHueScaleValues& a_hueScales,
            const WeatherPatcher::HueRanges& a_hueRanges,
            const std::array<bool, kFieldCount>& a_active)
        {
            for (std::size_t field = 0; field < kFieldCount; ++field)
            {
                if (!a_active[field])
                {
                    continue;
                }
                ForEachFieldColor(a_data, a_ambientColors, field, [&](RE::Color& a_color)
                    {
                        const double multiplier = a_settings.values[field] *
                                                  (UsesAmbientHueScales(field, a_settings) ? HueScale(a_color, a_hueScales, a_hueRanges) : 1.0);
                        if (std::abs(multiplier - 1.0) > 0.0001)
                        {
                            SaturateColor(a_color, multiplier);
                        } });
            }
        }

        void ApplyHueShift(
            RE::INTERIOR_DATA& a_data,
            RE::BGSDirectionalAmbientLightingColors& a_ambientColors,
            const InteriorHueShiftResolution& a_settings,
            const WeatherPatcher::HueRanges& a_hueRanges,
            const std::array<bool, kFieldCount>& a_active)
        {
            for (std::size_t field = 0; field < kFieldCount; ++field)
            {
                if (!a_active[field])
                {
                    continue;
                }
                ForEachFieldColor(a_data, a_ambientColors, field, [&](RE::Color& a_color)
                    {
                        const auto degrees = WeatherPatcher::ColorHueShiftDegrees(
                            a_color,
                            a_settings.values[field],
                            a_hueRanges);
                        if (std::abs(degrees) > 0.0001)
                        {
                            WeatherPatcher::ShiftHue(a_color, degrees);
                        } });
            }
        }

        Baseline MakeBaseline(
            const RE::INTERIOR_DATA& a_data,
            const RE::BGSDirectionalAmbientLightingColors& a_ambientColors)
        {
            return {
                .ambient = a_data.ambient,
                .directional = a_data.directional,
                .ambientColors = a_ambientColors,
                .fogFar = a_data.fogColorFar,
                .fogNear = a_data.fogColorNear,
                .fogMax = a_data.fogClamp,
            };
        }

        void RestoreBaseline(
            RE::INTERIOR_DATA& a_data,
            RE::BGSDirectionalAmbientLightingColors& a_ambientColors,
            const Baseline& a_baseline)
        {
            a_data.ambient = a_baseline.ambient;
            a_data.directional = a_baseline.directional;
            a_ambientColors = a_baseline.ambientColors;
            a_data.fogColorFar = a_baseline.fogFar;
            a_data.fogColorNear = a_baseline.fogNear;
            a_data.fogClamp = a_baseline.fogMax;
        }

        void RestoreAllCapturedBaselines()
        {
            auto* stat = Config::StatData::GetSingleton();
            for (const auto& [lightingTemplate, baseline] : stat->lightingTemplateBaselines)
            {
                if (lightingTemplate)
                {
                    RestoreBaseline(
                        lightingTemplate->data,
                        lightingTemplate->directionalAmbientLightingColors,
                        baseline);
                }
            }
            for (const auto& [cell, baseline] : stat->cellLightingBaselines)
            {
                if (!cell || !cell->IsInteriorCell())
                {
                    continue;
                }
                if (auto* interior = cell->GetRuntimeData().cellData.interior)
                {
                    RestoreBaseline(*interior, interior->directionalAmbientLightingColors, baseline);
                }
            }
        }

        std::array<bool, kFieldCount> CellActiveFields(const RE::INTERIOR_DATA& a_data)
        {
            const auto& inherit = a_data.lightingTemplateInheritanceFlags;
            return {
                (inherit & RE::INTERIOR_DATA::Inherit::kAmbientColor).underlying() == 0,
                (inherit & RE::INTERIOR_DATA::Inherit::kDirectionalColor).underlying() == 0,
                true,
                (inherit & RE::INTERIOR_DATA::Inherit::kFogColor).underlying() == 0,
                (inherit & RE::INTERIOR_DATA::Inherit::kFogColor).underlying() == 0,
            };
        }

        bool CellFogMaxActive(const RE::INTERIOR_DATA& a_data)
        {
            return (a_data.lightingTemplateInheritanceFlags & RE::INTERIOR_DATA::Inherit::kFogMax).underlying() == 0;
        }

    }  // namespace

    void CaptureCellBaseline(RE::TESObjectCELL* a_cell)
    {
        if (!retainRuntimeState.load(std::memory_order_relaxed) || !a_cell || !a_cell->IsInteriorCell())
        {
            return;
        }
        auto* interior = a_cell->GetRuntimeData().cellData.interior;
        if (!interior)
        {
            return;
        }

        Config::StatData::GetSingleton()->cellLightingBaselines.try_emplace(
            a_cell,
            MakeBaseline(*interior, interior->directionalAmbientLightingColors));
    }

    void ApplyDataLoaded()
    {
        retainRuntimeState.store(true, std::memory_order_relaxed);
        ApplyAllSettings();
    }

    void ReleaseRuntimeState()
    {
        retainRuntimeState.store(false, std::memory_order_relaxed);
        auto* stat = Config::StatData::GetSingleton();
        stat->lightingTemplateBaselines = {};
        stat->cellLightingBaselines = {};
        PointLightPatcher::ReleaseRuntimeState();
    }

    namespace
    {
        std::size_t ApplyLightingTemplates(
            const Settings& a_settings,
            const std::span<RE::BGSLightingTemplate* const> a_templates)
        {
            auto* stat = Config::StatData::GetSingleton();
            const auto links = ResolveInteriorLinks(a_settings.links.interior);
            const auto brightness = ResolveCategory(a_settings.intBrightnessMultiplier, links);
            const auto saturation = ResolveCategory(a_settings.intSaturationMultiplier, links);
            const auto hueShift = ResolveHueShiftCategory(a_settings.intHueShift, links);
            const auto hueScales = WeatherPatcher::ResolveHueScales(a_settings.intAmbientHueScales);
            constexpr std::array<bool, kFieldCount> allFields{ true, true, true, true, true };

            std::size_t count = 0;
            for (auto* lightingTemplate : a_templates)
            {
                if (!lightingTemplate)
                {
                    continue;
                }
                const auto baseline = stat->lightingTemplateBaselines
                                          .try_emplace(
                                              lightingTemplate,
                                              MakeBaseline(
                                                  lightingTemplate->data,
                                                  lightingTemplate->directionalAmbientLightingColors))
                                          .first;
                RestoreBaseline(
                    lightingTemplate->data,
                    lightingTemplate->directionalAmbientLightingColors,
                    baseline->second);
                ApplyBrightness(
                    lightingTemplate->data,
                    lightingTemplate->directionalAmbientLightingColors,
                    brightness,
                    allFields);
                ApplyFogMax(lightingTemplate->data, a_settings.intFogMaxMultiplier, true);
                ApplySaturation(
                    lightingTemplate->data,
                    lightingTemplate->directionalAmbientLightingColors,
                    saturation,
                    hueScales,
                    a_settings.intHueRanges,
                    allFields);
                ApplyHueShift(
                    lightingTemplate->data,
                    lightingTemplate->directionalAmbientLightingColors,
                    hueShift,
                    a_settings.intHueRanges,
                    allFields);
                ++count;
            }
            return count;
        }

        std::size_t ApplyInteriorCells(const Settings& a_settings)
        {
            auto* stat = Config::StatData::GetSingleton();
            const auto links = ResolveInteriorLinks(a_settings.links.interior);
            const auto brightness = ResolveCategory(a_settings.intBrightnessMultiplier, links);
            const auto saturation = ResolveCategory(a_settings.intSaturationMultiplier, links);
            const auto hueShift = ResolveHueShiftCategory(a_settings.intHueShift, links);
            const auto hueScales = WeatherPatcher::ResolveHueScales(a_settings.intAmbientHueScales);

            std::size_t count = 0;
            for (auto& [cell, baseline] : stat->cellLightingBaselines)
            {
                if (!cell || !cell->IsInteriorCell())
                {
                    continue;
                }
                auto* interior = cell->GetRuntimeData().cellData.interior;
                if (!interior)
                {
                    continue;
                }

                RestoreBaseline(*interior, interior->directionalAmbientLightingColors, baseline);
                const auto activeFields = CellActiveFields(*interior);
                ApplyBrightness(*interior, interior->directionalAmbientLightingColors, brightness, activeFields);
                ApplyFogMax(*interior, a_settings.intFogMaxMultiplier, CellFogMaxActive(*interior));
                ApplySaturation(
                    *interior,
                    interior->directionalAmbientLightingColors,
                    saturation,
                    hueScales,
                    a_settings.intHueRanges,
                    activeFields);
                ApplyHueShift(
                    *interior,
                    interior->directionalAmbientLightingColors,
                    hueShift,
                    a_settings.intHueRanges,
                    activeFields);
                ++count;
            }
            return count;
        }

        struct ActiveTemplateProfile
        {
            std::string name;
            RecordFilter::Resolved filter;
        };

        std::vector<ActiveTemplateProfile> GetActiveTemplateProfiles()
        {
            static constexpr std::array roots{
                std::string_view{ "intBrightnessMultiplier" },
                std::string_view{ "intSaturationMultiplier" },
                std::string_view{ "intHueShift" },
                std::string_view{ "intAmbientHueScales" },
                std::string_view{ "intHueRanges" },
                std::string_view{ "intFogMaxMultiplier" },
                std::string_view{ "lightingTemplateInclusions" },
                std::string_view{ "lightingTemplateExclusions" },
                std::string_view{ "lightingTemplatePluginInclusions" },
                std::string_view{ "lightingTemplatePluginExclusions" },
            };
            std::vector<ActiveTemplateProfile> result;
            for (auto profile : TuningUtil::GetProfilesWithSettings(roots))
            {
                const auto& settings = TuningUtil::GetSettings(profile);
                if (!settings.EnableProfile)
                {
                    continue;
                }
                result.push_back({
                    std::move(profile),
                    RecordFilter::Resolve(
                        settings.lightingTemplateInclusions,
                        settings.lightingTemplateExclusions,
                        settings.lightingTemplatePluginInclusions,
                        settings.lightingTemplatePluginExclusions),
                });
            }
            return result;
        }
    }  // namespace

    void ApplyAllSettings(const bool a_commitLightPlacer)
    {
        RestoreAllCapturedBaselines();
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("TESDataHandler is unavailable; Lighting settings were not applied");
            return;
        }

        static constexpr std::array cellRoots{
            std::string_view{ "intBrightnessMultiplier" },
            std::string_view{ "intSaturationMultiplier" },
            std::string_view{ "intHueShift" },
            std::string_view{ "intAmbientHueScales" },
            std::string_view{ "intHueRanges" },
            std::string_view{ "intFogMaxMultiplier" },
        };
        std::vector<std::string> activeCellProfiles;
        for (auto profile : TuningUtil::GetProfilesWithSettings(cellRoots))
        {
            if (TuningUtil::GetSettings(profile).EnableProfile)
            {
                activeCellProfiles.push_back(std::move(profile));
            }
        }
        if (!activeCellProfiles.empty())
        {
            const auto cellCount = ApplyInteriorCells(
                TuningUtil::ResolveSettingsStack(activeCellProfiles));
            DetailedLogging::Info(
                "Applied {} stacked Lighting profile(s) to {} direct interior cell record(s)",
                activeCellProfiles.size(),
                cellCount);
        }

        const auto templateProfiles = GetActiveTemplateProfiles();
        std::unordered_map<std::string, std::vector<RE::BGSLightingTemplate*>> templateGroups;
        std::unordered_map<std::string, std::vector<std::string>> groupProfiles;
        std::unordered_map<std::string, std::size_t> profileTargetCounts;
        for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
        {
            if (!lightingTemplate)
            {
                continue;
            }
            std::string signature;
            std::vector<std::string> matchingProfiles;
            for (const auto& profile : templateProfiles)
            {
                if (RecordFilter::Matches(lightingTemplate, profile.filter))
                {
                    matchingProfiles.push_back(profile.name);
                    signature.append(profile.name).push_back('\x1F');
                    ++profileTargetCounts[profile.name];
                }
            }
            if (!matchingProfiles.empty())
            {
                templateGroups[signature].push_back(lightingTemplate);
                groupProfiles.try_emplace(signature, std::move(matchingProfiles));
            }
        }

        std::size_t templateCount = 0;
        for (const auto& [signature, templates] : templateGroups)
        {
            templateCount += ApplyLightingTemplates(
                TuningUtil::ResolveSettingsStack(groupProfiles[signature]),
                templates);
        }
        for (const auto& profile : templateProfiles)
        {
            DetailedLogging::Info(
                "Lighting Template filter for profile {} matched {} template record(s)",
                profile.name,
                profileTargetCounts[profile.name]);
        }
        logger::info(
            "Applied {} Lighting Template profile stack(s) to {} template record(s)",
            templateGroups.size(),
            templateCount);

        static constexpr std::array pointLightRoots{
            std::string_view{ "pointLights" },
            std::string_view{ "intHueRanges" },
        };
        std::vector<std::string> pointLightProfiles;
        for (auto profile : TuningUtil::GetProfilesWithSettings(pointLightRoots))
        {
            if (TuningUtil::GetSettings(profile).EnableProfile)
            {
                pointLightProfiles.push_back(std::move(profile));
            }
        }
        const auto pointLightSettings = TuningUtil::ResolveSettingsStack(pointLightProfiles);
        PointLightPatcher::Apply(
            pointLightSettings.pointLights,
            pointLightSettings.intHueRanges,
            a_commitLightPlacer);
    }

}  // namespace MPL::LightingPatcher
