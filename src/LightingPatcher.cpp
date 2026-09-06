#include <Config.h>
#include <DetailedLogging.h>
#include <LightingPatcher.h>
#include <PointLightPatcher.h>
#include <RecordFilter.h>
#include <SettingLinks.h>
#include <SliderLinkMath.h>
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
#include <unordered_set>

namespace MPL::LightingPatcher
{
    namespace
    {
        std::atomic_bool retainRuntimeState{ true };
        constexpr std::size_t kFieldCount = 5;
        constexpr double kZeroFogMaxBaseline = 0.1;
        constexpr std::array<std::string_view, kFieldCount> kFieldNames{
            "ambient", "directional", "ambientColors", "fogFar", "fogNear"
        };
        using TemplateInheritFlags = REX::EnumSet<RE::INTERIOR_DATA::Inherit, std::uint32_t>;
        constexpr std::array<std::pair<std::string_view, RE::INTERIOR_DATA::Inherit>, 11> kTemplateInheritFlags{
            std::pair{ "ambientColor", RE::INTERIOR_DATA::Inherit::kAmbientColor },
            std::pair{ "directionalColor", RE::INTERIOR_DATA::Inherit::kDirectionalColor },
            std::pair{ "fogColor", RE::INTERIOR_DATA::Inherit::kFogColor },
            std::pair{ "fogNear", RE::INTERIOR_DATA::Inherit::kFogNear },
            std::pair{ "fogFar", RE::INTERIOR_DATA::Inherit::kFogFar },
            std::pair{ "directionalRotation", RE::INTERIOR_DATA::Inherit::kDirectionalRotation },
            std::pair{ "directionalFade", RE::INTERIOR_DATA::Inherit::kDirectionalFade },
            std::pair{ "clipDistance", RE::INTERIOR_DATA::Inherit::kClipDistance },
            std::pair{ "fogPower", RE::INTERIOR_DATA::Inherit::kFogPower },
            std::pair{ "fogMax", RE::INTERIOR_DATA::Inherit::kFogMax },
            std::pair{ "lightFadeDistances", RE::INTERIOR_DATA::Inherit::kLightFadeDistances },
        };
        std::vector<std::string> startupTemplateDrivenProfiles;
        std::unordered_map<std::string, std::unordered_set<RE::FormID>>
            startupFilteredLocationTypeTemplateInclusions;
        std::unordered_map<std::string, std::unordered_set<RE::FormID>>
            startupFilteredLocationTypeTemplateExclusions;
        struct CachedLightingTemplateLocationFilter
        {
            TuningUtil::LightingTemplateFilter configured;
            std::unordered_set<RE::FormID> includedFormIDs;
            std::unordered_set<RE::FormID> excludedFormIDs;
        };
        std::unordered_map<std::string, CachedLightingTemplateLocationFilter>
            lightingTemplateLocationFilters;

        std::string NormalizeProfileName(std::string_view a_name)
        {
            std::string result(a_name);
            std::ranges::transform(
                result,
                result.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return result;
        }

        std::string FilteredLocationTypeFilterKey(
            const std::string_view a_profileName,
            const std::string_view a_ruleID)
        {
            return NormalizeProfileName(a_profileName)
                .append("\x1F")
                .append(NormalizeProfileName(a_ruleID));
        }

        struct Resolution
        {
            std::array<double, kFieldCount> values{};
            LightingLinkTopology links{};
        };

        struct LightingHueShiftResolution
        {
            std::array<WeatherPatcher::HueShiftBands, kFieldCount> values{};
        };

        struct LightingBrightnessResolution
        {
            Resolution linked;
            std::array<double, kFieldCount> direct;
        };

        LightingLinkTopology ResolveCategoryLinks(
            const LightingLinks& a_links,
            const std::span<const std::string> a_profileNames,
            const std::string_view a_settingRoot)
        {
            return ResolveLightingLinks(TuningUtil::ResolveLightingSliderLinks(
                a_profileNames,
                a_settingRoot,
                a_links));
        }

        Resolution ResolveCategory(
            const std::array<double, kFieldCount>& a_values,
            const LightingLinkTopology& a_links)
        {
            Resolution result{
                .values = a_values,
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

        Resolution ResolveCategory(
            const LightingColorSettings& a_settings,
            const LightingLinkTopology& a_links)
        {
            return ResolveCategory(std::array{ a_settings.ambient, a_settings.directional, a_settings.ambientColors,
                                       a_settings.fogFar, a_settings.fogNear }, a_links);
        }

        LightingBrightnessResolution ResolveBrightnessCategory(
            const Settings& a_settings,
            const std::span<const std::string> a_profileNames)
        {
            const auto& brightness = a_settings.lightBrightnessMultiplier;
            std::array values{ brightness.ambient, brightness.directional, brightness.ambientColors,
                brightness.fogFar, brightness.fogNear };
            std::array<std::optional<LightingLinkTopology>, kFieldCount> overrides;
            const auto customLinks = TuningUtil::ResolveLightingSliderLinkOverrides(a_profileNames, "lightBrightnessMultiplier");
            for (std::size_t field = 0; field < kFieldCount; ++field)
            {
                if (const auto custom = customLinks.find(NormalizeProfileName(kFieldNames[field])); custom != customLinks.end())
                    overrides[field] = ResolveLightingLinks(custom->second);
            }
            const auto direct = SliderLinkMath::SeparateContributions(values, overrides);
            return { ResolveCategory(values, ResolveLightingLinks(a_settings.links.lighting)), direct };
        }

        LightingHueShiftResolution ResolveHueShiftCategory(
            const LightingHueShiftSettings& a_settings,
            const LightingLinkTopology& a_links)
        {
            LightingHueShiftResolution result{ .values = {
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
            const std::array<bool, kFieldCount>& a_active,
            const std::array<double, kFieldCount>* a_directMultipliers = nullptr)
        {
            const auto gains = SliderLinkMath::ResolveBrightnessGains(
                a_settings.values, a_settings.links, a_directMultipliers,
                [&](const std::size_t a_field, const double a_multiplier)
                { return ConstrainFieldGain(a_data, a_ambientColors, a_field, a_multiplier); });
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
            if (!a_active)
            {
                return;
            }
            const auto useZeroBaseline = a_data.fogClamp == 0.0f && multiplier > 0.0;
            if (!useZeroBaseline && std::abs(multiplier - 1.0) <= 0.0001)
            {
                return;
            }
            const auto baseline = useZeroBaseline ? kZeroFogMaxBaseline : static_cast<double>(a_data.fogClamp);
            const auto value = baseline * multiplier;
            a_data.fogClamp = static_cast<float>(std::clamp(
                value,
                -static_cast<double>(std::numeric_limits<float>::max()),
                static_cast<double>(std::numeric_limits<float>::max())));
        }

        void ApplyFogPower(RE::INTERIOR_DATA& a_data, const double a_multiplier, const bool a_active)
        {
            if (!a_active)
            {
                return;
            }
            const auto value = static_cast<double>(a_data.fogPower) * std::max(0.0, a_multiplier);
            a_data.fogPower = static_cast<float>(std::clamp(
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
            const LightingHueShiftResolution& a_settings,
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
                .fogPower = a_data.fogPower,
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
            a_data.fogPower = a_baseline.fogPower;
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
                if (auto* lightingData = cell->GetRuntimeData().cellData.interior)
                {
                    RestoreBaseline(*lightingData, lightingData->directionalAmbientLightingColors, baseline);
                }
            }
        }

        std::array<bool, kFieldCount> CellActiveFields(const RE::INTERIOR_DATA& a_data)
        {
            const auto& inherit = a_data.lightingTemplateInheritanceFlags;
            const auto ambientActive =
                (inherit & RE::INTERIOR_DATA::Inherit::kAmbientColor).underlying() == 0;
            return {
                ambientActive,
                (inherit & RE::INTERIOR_DATA::Inherit::kDirectionalColor).underlying() == 0,
                ambientActive,
                (inherit & RE::INTERIOR_DATA::Inherit::kFogColor).underlying() == 0,
                (inherit & RE::INTERIOR_DATA::Inherit::kFogColor).underlying() == 0,
            };
        }

        bool CellFogMaxActive(const RE::INTERIOR_DATA& a_data)
        {
            return (a_data.lightingTemplateInheritanceFlags & RE::INTERIOR_DATA::Inherit::kFogMax).underlying() == 0;
        }

        bool CellFogPowerActive(const RE::INTERIOR_DATA& a_data)
        {
            return (a_data.lightingTemplateInheritanceFlags & RE::INTERIOR_DATA::Inherit::kFogPower).underlying() == 0;
        }

        void BuildStartupFilteredLocationTypeFilters();
        void ApplyStartupCellSettings();

    }  // namespace

    static void CaptureCellBaseline(RE::TESObjectCELL* a_cell)
    {
        if (!retainRuntimeState.load(std::memory_order_relaxed) || !a_cell || !a_cell->IsInteriorCell())
        {
            return;
        }
        auto* lightingData = a_cell->GetRuntimeData().cellData.interior;
        if (!lightingData)
        {
            return;
        }

        Config::StatData::GetSingleton()->cellLightingBaselines.try_emplace(
            a_cell,
            MakeBaseline(*lightingData, lightingData->directionalAmbientLightingColors));
    }

    void ApplyDataLoaded()
    {
        retainRuntimeState.store(true, std::memory_order_relaxed);
        BuildStartupFilteredLocationTypeFilters();
        ApplyStartupCellSettings();
        ApplyAllSettings();
    }

    void ReleaseRuntimeState()
    {
        retainRuntimeState.store(false, std::memory_order_relaxed);
        auto* stat = Config::StatData::GetSingleton();
        stat->lightingTemplateBaselines = {};
        stat->cellLightingBaselines = {};
        startupTemplateDrivenProfiles.clear();
        startupFilteredLocationTypeTemplateInclusions.clear();
        startupFilteredLocationTypeTemplateExclusions.clear();
        lightingTemplateLocationFilters.clear();
        PointLightPatcher::ReleaseRuntimeState();
    }

    namespace
    {
        struct FilteredLightingTemplateAdjustments
        {
            std::array<double, kFieldCount> brightness{ 1.0, 1.0, 1.0, 1.0, 1.0 };
            std::array<double, kFieldCount> directBrightness{ 1.0, 1.0, 1.0, 1.0, 1.0 };
            double fogPower = 1.0;
            double fogStrength = 1.0;
        };

        struct LocationTypeTemplateEvidence
        {
            RE::BGSLightingTemplate* lightingTemplate = nullptr;
            RE::TESObjectCELL* cell = nullptr;
            RE::BGSLocation* location = nullptr;
            RE::BGSKeyword* keyword = nullptr;
        };

        RecordFilter::Resolved ResolveFilteredLightingTemplateFilter(
            const TuningUtil::FilteredLightingTemplateRule& a_rule,
            const std::string_view a_profileName)
        {
            const TuningUtil::PluginFilter noPlugins;
            auto result = RecordFilter::Resolve(a_rule.include, a_rule.exclude, noPlugins, noPlugins);
            result.requireIncludedRecordMatch = !a_rule.locationTypeInclusions.empty();
            auto explicitlyListedFormIDs = result.includedFormIDs;
            explicitlyListedFormIDs.insert(result.excludedFormIDs.begin(), result.excludedFormIDs.end());
            const auto addLocationMatches = [&](
                                                std::unordered_set<RE::FormID>& a_target,
                                                const std::unordered_set<RE::FormID>& a_matches)
            {
                for (const auto formID : a_matches)
                {
                    if (!explicitlyListedFormIDs.contains(formID))
                    {
                        a_target.insert(formID);
                    }
                }
            };
            const auto key = FilteredLocationTypeFilterKey(a_profileName, a_rule.id);
            if (const auto found = startupFilteredLocationTypeTemplateInclusions.find(key);
                found != startupFilteredLocationTypeTemplateInclusions.end())
            {
                addLocationMatches(result.includedFormIDs, found->second);
            }
            if (const auto found = startupFilteredLocationTypeTemplateExclusions.find(
                    key);
                found != startupFilteredLocationTypeTemplateExclusions.end())
            {
                addLocationMatches(result.excludedFormIDs, found->second);
            }
            return result;
        }

        std::vector<RE::BGSKeyword*> ResolveLocationTypeKeywords(
            const std::string_view a_owner,
            const std::span<const std::string> a_selectors,
            const std::string_view a_filterKind)
        {
            std::vector<RE::BGSKeyword*> keywords;
            std::unordered_set<RE::FormID> keywordFormIDs;
            for (const auto& selector : a_selectors)
            {
                const auto formID = Config::LiteForm::FromString(selector).formID;
                auto* keyword = formID ? RE::TESForm::LookupByID<RE::BGSKeyword>(formID) : nullptr;
                if (!keyword)
                {
                    logger::warn(
                        "[Lighting Template] {} {} | keyword='{}' invalid",
                        a_filterKind,
                        a_owner,
                        selector);
                    continue;
                }
                if (keywordFormIDs.insert(formID).second)
                {
                    keywords.push_back(keyword);
                }
            }
            return keywords;
        }

        std::unordered_set<RE::FormID> BuildLocationTypeTemplateSet(
            const std::string_view a_owner,
            const std::span<const std::string> a_selectors,
            const std::span<const std::string> a_multiLocationExceptions,
            RE::TESDataHandler* a_dataHandler,
            const bool a_inclusion)
        {
            const auto filterKind = a_inclusion ? "inclusion" : "exclusion";
            const auto keywords = ResolveLocationTypeKeywords(a_owner, a_selectors, filterKind);
            const auto exceptionKeywords = ResolveLocationTypeKeywords(
                a_owner,
                a_multiLocationExceptions,
                std::format("{} multi-location exception", filterKind));

            std::unordered_map<RE::FormID, LocationTypeTemplateEvidence> evidenceByTemplate;
            std::size_t matchingCellCount = 0;
            std::size_t exceptionCellCount = 0;
            for (auto* cell : a_dataHandler->interiorCells)
            {
                auto* lightingTemplate = cell ? cell->GetRuntimeData().lightingTemplate : nullptr;
                auto* location = cell ? cell->GetLocation() : nullptr;
                if (!lightingTemplate || !location)
                {
                    continue;
                }
                const auto matchedKeyword = std::ranges::find_if(
                    keywords,
                    [&](const RE::BGSKeyword* a_keyword)
                    {
                        return a_keyword && location->HasKeyword(a_keyword);
                    });
                if (matchedKeyword == keywords.end())
                {
                    continue;
                }
                if (std::ranges::any_of(
                        exceptionKeywords,
                        [&](const RE::BGSKeyword* a_keyword)
                        {
                            return a_keyword && location->HasKeyword(a_keyword);
                        }))
                {
                    ++exceptionCellCount;
                    continue;
                }

                ++matchingCellCount;
                const auto templateFormID = lightingTemplate->GetFormID();
                evidenceByTemplate.try_emplace(
                    templateFormID,
                    LocationTypeTemplateEvidence{
                        .lightingTemplate = lightingTemplate,
                        .cell = cell,
                        .location = location,
                        .keyword = *matchedKeyword,
                    });
            }

            std::unordered_set<RE::FormID> result;
            for (const auto& [templateFormID, evidence] : evidenceByTemplate)
            {
                result.insert(templateFormID);
                logger::info(
                    "{} {} | {} | cell={} | location={} | keyword={}",
                    a_owner,
                    a_inclusion ? "include" : "exclude",
                    RecordFilter::FormKey(evidence.lightingTemplate),
                    RecordFilter::FormKey(evidence.cell),
                    RecordFilter::FormKey(evidence.location),
                    RecordFilter::FormKey(evidence.keyword));
            }
            logger::info(
                "[Lighting Template] {} {} | keywords={} | exceptions={} | cells={} | bypassed={} | templates={}",
                a_owner,
                a_inclusion ? "include" : "exclude",
                keywords.size(),
                exceptionKeywords.size(),
                matchingCellCount,
                exceptionCellCount,
                result.size());
            return result;
        }

        void BuildStartupFilteredLocationTypeFilters()
        {
            startupFilteredLocationTypeTemplateInclusions.clear();
            startupFilteredLocationTypeTemplateExclusions.clear();
            lightingTemplateLocationFilters.clear();
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                logger::warn("[Lighting Template] filters unresolved | TESDataHandler unavailable");
                return;
            }

            for (const auto& discovered : TuningUtil::GetProfiles())
            {
                const auto& profileName = discovered.name;
                for (const auto& rule : discovered.filteredLightingTemplateRules)
                {
                    const auto key = FilteredLocationTypeFilterKey(profileName, rule.id);
                    const auto owner = profileName + "/" + rule.id;
                    if (!rule.locationTypeInclusions.empty())
                    {
                        startupFilteredLocationTypeTemplateInclusions.insert_or_assign(
                            key,
                            BuildLocationTypeTemplateSet(
                                owner,
                                rule.locationTypeInclusions,
                                rule.inclusionMultiLocationExceptions,
                                dataHandler,
                                true));
                    }
                    if (!rule.locationTypeExclusions.empty())
                    {
                        startupFilteredLocationTypeTemplateExclusions.insert_or_assign(
                            key,
                            BuildLocationTypeTemplateSet(
                                owner,
                                rule.locationTypeExclusions,
                                rule.exclusionMultiLocationExceptions,
                                dataHandler,
                                false));
                    }
                }

                auto profileNameCopy = profileName;
                const auto& settings = TuningUtil::GetSettings(profileNameCopy);
                CachedLightingTemplateLocationFilter cache{
                    .configured = settings.lightingTemplateFilter,
                };
                if (!cache.configured.include.locationTypes.empty())
                {
                    cache.includedFormIDs = BuildLocationTypeTemplateSet(
                        profileName,
                        cache.configured.include.locationTypes,
                        cache.configured.include.multiLocationExceptions,
                        dataHandler,
                        true);
                }
                if (!cache.configured.exclude.locationTypes.empty())
                {
                    cache.excludedFormIDs = BuildLocationTypeTemplateSet(
                        profileName,
                        cache.configured.exclude.locationTypes,
                        cache.configured.exclude.multiLocationExceptions,
                        dataHandler,
                        false);
                }
                lightingTemplateLocationFilters.insert_or_assign(
                    NormalizeProfileName(profileName),
                    std::move(cache));
            }
        }

        RecordFilter::Resolved ResolveLightingTemplateFilter(
            const Settings& a_settings,
            const std::string_view a_profileName)
        {
            auto result = RecordFilter::Resolve(
                a_settings.lightingTemplateInclusions,
                a_settings.lightingTemplateExclusions,
                a_settings.lightingTemplatePluginInclusions,
                a_settings.lightingTemplatePluginExclusions);
            result.requireIncludedRecordMatch =
                !a_settings.lightingTemplateFilter.include.locationTypes.empty();

            const auto key = NormalizeProfileName(a_profileName);
            auto cached = lightingTemplateLocationFilters.find(key);
            if (cached == lightingTemplateLocationFilters.end() ||
                cached->second.configured != a_settings.lightingTemplateFilter)
            {
                CachedLightingTemplateLocationFilter replacement{
                    .configured = a_settings.lightingTemplateFilter,
                };
                if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
                {
                    if (!replacement.configured.include.locationTypes.empty())
                    {
                        replacement.includedFormIDs = BuildLocationTypeTemplateSet(
                            a_profileName,
                            replacement.configured.include.locationTypes,
                            replacement.configured.include.multiLocationExceptions,
                            dataHandler,
                            true);
                    }
                    if (!replacement.configured.exclude.locationTypes.empty())
                    {
                        replacement.excludedFormIDs = BuildLocationTypeTemplateSet(
                            a_profileName,
                            replacement.configured.exclude.locationTypes,
                            replacement.configured.exclude.multiLocationExceptions,
                            dataHandler,
                            false);
                    }
                }
                cached = lightingTemplateLocationFilters.insert_or_assign(
                    key,
                    std::move(replacement)).first;
            }

            auto explicitlyListedFormIDs = result.includedFormIDs;
            explicitlyListedFormIDs.insert(
                result.excludedFormIDs.begin(),
                result.excludedFormIDs.end());
            const auto addLocationMatches = [&](
                                                std::unordered_set<RE::FormID>& a_target,
                                                const std::unordered_set<RE::FormID>& a_matches)
            {
                for (const auto formID : a_matches)
                {
                    if (!explicitlyListedFormIDs.contains(formID))
                    {
                        a_target.insert(formID);
                    }
                }
            };
            addLocationMatches(result.includedFormIDs, cached->second.includedFormIDs);
            addLocationMatches(result.excludedFormIDs, cached->second.excludedFormIDs);
            return result;
        }

        std::size_t ApplyLightingTemplates(
            const Settings& a_settings,
            const std::span<const std::string> a_profileNames,
            const std::span<RE::BGSLightingTemplate* const> a_templates,
            const FilteredLightingTemplateAdjustments& a_filtered)
        {
            auto* stat = Config::StatData::GetSingleton();
            const auto saturationLinks = ResolveCategoryLinks(
                a_settings.links.lighting,
                a_profileNames,
                "lightSaturationMultiplier");
            const auto hueShiftLinks = ResolveCategoryLinks(
                a_settings.links.lighting,
                a_profileNames,
                "lightHueShift");
            auto brightness = ResolveBrightnessCategory(a_settings, a_profileNames);
            for (std::size_t field = 0; field < kFieldCount; ++field)
            {
                brightness.linked.values[field] *= a_filtered.brightness[field];
                brightness.direct[field] *= a_filtered.directBrightness[field];
            }
            const auto saturation = ResolveCategory(a_settings.lightSaturationMultiplier, saturationLinks);
            const auto hueShift = ResolveHueShiftCategory(a_settings.lightHueShift, hueShiftLinks);
            const auto hueScales = WeatherPatcher::ResolveHueScales(a_settings.lightAmbientHueScales);
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
                    brightness.linked,
                    allFields,
                    std::addressof(brightness.direct));
                ApplyFogMax(
                    lightingTemplate->data,
                    a_settings.lightFogMaxMultiplier * a_filtered.fogStrength,
                    true);
                ApplyFogPower(
                    lightingTemplate->data,
                    a_settings.lightFogPowerMultiplier * a_filtered.fogPower,
                    true);
                ApplySaturation(
                    lightingTemplate->data,
                    lightingTemplate->directionalAmbientLightingColors,
                    saturation,
                    hueScales,
                    a_settings.lightHueRanges,
                    allFields);
                ApplyHueShift(
                    lightingTemplate->data,
                    lightingTemplate->directionalAmbientLightingColors,
                    hueShift,
                    a_settings.lightHueRanges,
                    allFields);
                ++count;
            }
            return count;
        }

        std::size_t ApplyLightingCells(
            const Settings& a_settings,
            const std::span<const std::string> a_profileNames)
        {
            auto* stat = Config::StatData::GetSingleton();
            const auto brightness = ResolveBrightnessCategory(a_settings, a_profileNames);
            const auto saturation = ResolveCategory(
                a_settings.lightSaturationMultiplier,
                ResolveCategoryLinks(a_settings.links.lighting, a_profileNames, "lightSaturationMultiplier"));
            const auto hueShift = ResolveHueShiftCategory(
                a_settings.lightHueShift,
                ResolveCategoryLinks(a_settings.links.lighting, a_profileNames, "lightHueShift"));
            const auto hueScales = WeatherPatcher::ResolveHueScales(a_settings.lightAmbientHueScales);

            std::size_t count = 0;
            for (auto& [cell, baseline] : stat->cellLightingBaselines)
            {
                if (!cell || !cell->IsInteriorCell())
                {
                    continue;
                }
                auto* lightingData = cell->GetRuntimeData().cellData.interior;
                if (!lightingData)
                {
                    continue;
                }

                RestoreBaseline(*lightingData, lightingData->directionalAmbientLightingColors, baseline);
                const auto activeFields = CellActiveFields(*lightingData);
                ApplyBrightness(*lightingData, lightingData->directionalAmbientLightingColors,
                    brightness.linked, activeFields, std::addressof(brightness.direct));
                ApplyFogMax(*lightingData, a_settings.lightFogMaxMultiplier, CellFogMaxActive(*lightingData));
                ApplyFogPower(*lightingData, a_settings.lightFogPowerMultiplier, CellFogPowerActive(*lightingData));
                ApplySaturation(
                    *lightingData,
                    lightingData->directionalAmbientLightingColors,
                    saturation,
                    hueScales,
                    a_settings.lightHueRanges,
                    activeFields);
                ApplyHueShift(
                    *lightingData,
                    lightingData->directionalAmbientLightingColors,
                    hueShift,
                    a_settings.lightHueRanges,
                    activeFields);
                ++count;
            }
            return count;
        }

        TemplateInheritFlags ResolveTemplateInheritFlags(
            const std::span<const std::string> a_names,
            const std::string_view a_profileName)
        {
            TemplateInheritFlags result;
            for (const auto& name : a_names)
            {
                const auto match = std::ranges::find_if(
                    kTemplateInheritFlags,
                    [&](const auto& a_entry) { return Config::IEquals(name, a_entry.first); });
                if (match == kTemplateInheritFlags.end())
                {
                    logger::warn(
                        "[Lighting] {} inheritance | flag={} unknown",
                        a_profileName,
                        name);
                    continue;
                }
                result |= match->second;
            }
            return result;
        }

        struct ActiveTemplateInheritProfile
        {
            std::string name;
            TemplateInheritFlags flags;
            std::unordered_set<RE::FormID> excludedCellFormIDs;
            RecordFilter::Resolved lightingTemplateFilter;
        };

        std::unordered_set<RE::FormID> ResolveConfiguredFormIDs(
            const std::span<const std::string> a_configuredFormIDs)
        {
            std::unordered_set<RE::FormID> result;
            for (const auto& configured : a_configuredFormIDs)
            {
                const auto formID = Config::LiteForm::FromString(configured).formID;
                if (formID != 0)
                {
                    result.insert(formID);
                }
            }
            return result;
        }

        std::vector<ActiveTemplateInheritProfile> GetStartupTemplateInheritProfiles()
        {
            static constexpr std::array roots{ std::string_view{ "enableTemplateInherit" } };
            std::vector<ActiveTemplateInheritProfile> result;
            startupTemplateDrivenProfiles.clear();
            for (auto profile : TuningUtil::GetProfilesWithSettings(roots))
            {
                const auto& settings = TuningUtil::GetSettings(profile);
                const auto flags = ResolveTemplateInheritFlags(settings.enableTemplateInherit, profile);
                if (!flags)
                {
                    continue;
                }
                startupTemplateDrivenProfiles.push_back(profile);
                if (!settings.EnableProfile)
                {
                    continue;
                }
                auto lightingTemplateFilter = ResolveLightingTemplateFilter(
                    settings,
                    profile);
                result.push_back({
                    .name = std::move(profile),
                    .flags = flags,
                    .excludedCellFormIDs = ResolveConfiguredFormIDs(settings.cellExclusions),
                    .lightingTemplateFilter = std::move(lightingTemplateFilter),
                });
            }
            return result;
        }

        void LogTemplateInheritExclusion(
            const ActiveTemplateInheritProfile& a_profile,
            const RE::TESObjectCELL* a_cell)
        {
            if (!DetailedLogging::IsEnabled())
            {
                return;
            }
            DetailedLogging::Info(
                "[Lighting] {} inheritance exclude | cell={:08X} | source=cellExclusions",
                a_profile.name,
                a_cell->GetFormID());
        }

        bool UsesTemplateInheritance(const std::string_view a_profileName)
        {
            return std::ranges::any_of(startupTemplateDrivenProfiles, [&](const auto& a_profile)
                { return Config::IEquals(a_profileName, a_profile); });
        }

        std::vector<std::string> GetActiveDirectCellProfiles()
        {
            static constexpr std::array roots{
                std::string_view{ "lightBrightnessMultiplier" },
                std::string_view{ "lightSaturationMultiplier" },
                std::string_view{ "lightHueShift" },
                std::string_view{ "lightAmbientHueScales" },
                std::string_view{ "lightHueRanges" },
                std::string_view{ "lightFogPowerMultiplier" },
                std::string_view{ "lightFogMaxMultiplier" },
            };
            std::vector<std::string> result;
            for (auto profile : TuningUtil::GetProfilesWithSettings(roots))
            {
                if (TuningUtil::GetSettings(profile).EnableProfile &&
                    !UsesTemplateInheritance(profile))
                {
                    result.push_back(std::move(profile));
                }
            }
            return result;
        }

        void ApplyTemplateInherit(
            RE::TESObjectCELL* a_cell,
            const std::span<const ActiveTemplateInheritProfile> a_profiles,
            std::unordered_map<std::string, std::size_t>* a_profileTargetCounts = nullptr)
        {
            if (!a_cell || !a_cell->IsInteriorCell())
            {
                return;
            }
            auto* lightingData = a_cell->GetRuntimeData().cellData.interior;
            auto* lightingTemplate = a_cell->GetRuntimeData().lightingTemplate;
            if (!lightingData || !lightingTemplate)
            {
                return;
            }

            for (const auto& profile : a_profiles)
            {
                if (profile.excludedCellFormIDs.contains(a_cell->GetFormID()))
                {
                    LogTemplateInheritExclusion(profile, a_cell);
                    continue;
                }
                if (!RecordFilter::Matches(lightingTemplate, profile.lightingTemplateFilter))
                {
                    continue;
                }
                lightingData->lightingTemplateInheritanceFlags |= profile.flags;
                if (a_profileTargetCounts)
                {
                    ++(*a_profileTargetCounts)[profile.name];
                }
            }
        }

        void ApplyStartupCellSettings()
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                logger::warn("[Lighting] startup CELL apply failed | TESDataHandler unavailable");
                return;
            }

            const auto inheritProfiles = GetStartupTemplateInheritProfiles();
            std::unordered_map<std::string, std::size_t> inheritTargetCounts;
            for (auto* cell : dataHandler->interiorCells)
            {
                CaptureCellBaseline(cell);
                ApplyTemplateInherit(cell, inheritProfiles, std::addressof(inheritTargetCounts));
            }
            for (const auto& profile : inheritProfiles)
            {
                DetailedLogging::Info(
                    "[Lighting] {} inheritance | cells={}",
                    profile.name,
                    inheritTargetCounts[profile.name]);
            }
        }

        struct ActiveTemplateProfile
        {
            std::string name;
            Settings settings;
            RecordFilter::Resolved filter;
            RecordFilter::Resolved ownershipFilter;
            bool appliesStandardSettings = false;
            bool declaresOwnership = false;
            struct FilteredRule
            {
                const TuningUtil::FilteredLightingTemplateRule* rule = nullptr;
                RecordFilter::Resolved filter;
            };
            std::vector<FilteredRule> filteredRules;
        };

        double FilteredLightingTemplateValue(
            const Settings& a_settings,
            const TuningUtil::FilteredLightingTemplateRule& a_rule)
        {
            if (const auto exact = a_settings.filteredLightingTemplateAdjustments.find(a_rule.id);
                exact != a_settings.filteredLightingTemplateAdjustments.end())
            {
                return exact->second;
            }
            const auto insensitive = std::ranges::find_if(
                a_settings.filteredLightingTemplateAdjustments,
                [&](const auto& a_entry) { return Config::IEquals(a_entry.first, a_rule.id); });
            return insensitive != a_settings.filteredLightingTemplateAdjustments.end() ?
                       insensitive->second :
                       a_rule.defaultValue;
        }

        void AccumulateFilteredLightingTemplateAdjustments(
            FilteredLightingTemplateAdjustments& a_adjustments,
            const Settings& a_settings,
            const TuningUtil::FilteredLightingTemplateRule& a_rule)
        {
            const auto value = FilteredLightingTemplateValue(a_settings, a_rule);
            if (a_rule.customLinks &&
                std::ranges::all_of(a_rule.settings, [](const auto& a_setting)
                    { return a_setting.operation == TuningUtil::FilteredLightingTemplateOperation::brightness; }))
            {
                std::array<double, kFieldCount> values{ 1.0, 1.0, 1.0, 1.0, 1.0 };
                std::array<bool, kFieldCount> active{};
                for (const auto& setting : a_rule.settings)
                {
                    const auto field = std::ranges::find_if(kFieldNames, [&](const auto a_name)
                        { return Config::IEquals(a_name, setting.target); });
                    if (field == kFieldNames.end()) continue;
                    const auto index = static_cast<std::size_t>(std::distance(kFieldNames.begin(), field));
                    values[index] *= std::max(0.0, 1.0 + ((value - 1.0) * setting.scale));
                    active[index] = true;
                }

                const auto links = ResolveLightingLinks(*a_rule.customLinks);
                const auto contribution = SliderLinkMath::ResolveContribution(values, active, links);
                for (std::size_t field = 0; field < kFieldCount; ++field)
                    a_adjustments.directBrightness[field] *= contribution[field];
                return;
            }

            for (const auto& setting : a_rule.settings)
            {
                const auto multiplier = std::max(0.0, 1.0 + ((value - 1.0) * setting.scale));
                if (setting.operation == TuningUtil::FilteredLightingTemplateOperation::fogStrength)
                {
                    a_adjustments.fogStrength *= multiplier;
                    continue;
                }
                if (setting.operation == TuningUtil::FilteredLightingTemplateOperation::fogPower)
                {
                    a_adjustments.fogPower *= multiplier;
                    continue;
                }
                const auto field = std::ranges::find_if(kFieldNames, [&](const auto a_name)
                    { return Config::IEquals(a_name, setting.target); });
                if (field == kFieldNames.end()) continue;
                const auto index = static_cast<std::size_t>(std::distance(kFieldNames.begin(), field));
                a_adjustments.brightness[index] *= multiplier;
            }
        }

        std::vector<ActiveTemplateProfile> GetActiveTemplateProfiles()
        {
            static constexpr std::array roots{
                std::string_view{ "lightBrightnessMultiplier" },
                std::string_view{ "lightSaturationMultiplier" },
                std::string_view{ "lightHueShift" },
                std::string_view{ "lightAmbientHueScales" },
                std::string_view{ "lightHueRanges" },
                std::string_view{ "lightFogPowerMultiplier" },
                std::string_view{ "lightFogMaxMultiplier" },
                std::string_view{ "lightingTemplateInclusions" },
                std::string_view{ "lightingTemplateExclusions" },
                std::string_view{ "lightingTemplatePluginInclusions" },
                std::string_view{ "lightingTemplatePluginExclusions" },
                std::string_view{ "lightingTemplateFilter" },
            };
            std::vector<ActiveTemplateProfile> result;
            auto directProfiles = TuningUtil::GetProfilesWithSettings(roots);
            for (const auto& discovered : TuningUtil::GetProfiles())
            {
                const auto appliesStandardSettings = std::ranges::any_of(directProfiles, [&](const auto& a_profile)
                    { return Config::IEquals(a_profile, discovered.name); });
                const auto& filteredRules = TuningUtil::GetFilteredLightingTemplateRules(discovered.name);

                auto profile = discovered.name;
                const auto& settings = TuningUtil::GetSettings(profile);
                if (!settings.EnableProfile)
                {
                    continue;
                }
                const auto declaresOwnership =
                    !settings.lightingTemplatePluginOwnership.exact.empty() ||
                    !settings.lightingTemplatePluginOwnership.contains.empty();
                if (!appliesStandardSettings && filteredRules.empty() && !declaresOwnership)
                {
                    continue;
                }
                auto lightingTemplateFilter = ResolveLightingTemplateFilter(
                    settings,
                    profile);
                ActiveTemplateProfile active{
                    .name = std::move(profile),
                    .settings = settings,
                    .filter = std::move(lightingTemplateFilter),
                    .ownershipFilter = {
                        .includedPlugins = settings.lightingTemplatePluginOwnership,
                    },
                    .appliesStandardSettings = appliesStandardSettings,
                    .declaresOwnership = declaresOwnership,
                };
                for (const auto& rule : filteredRules)
                {
                    active.filteredRules.push_back({
                        .rule = std::addressof(rule),
                        .filter = ResolveFilteredLightingTemplateFilter(rule, active.name),
                    });
                }
                result.push_back(std::move(active));
            }
            return result;
        }

        const ActiveTemplateProfile* LightingTemplateOwner(
            const RE::BGSLightingTemplate* a_lightingTemplate,
            const std::span<const ActiveTemplateProfile> a_profiles)
        {
            const ActiveTemplateProfile* owner = nullptr;
            for (const auto& profile : a_profiles)
            {
                if (profile.declaresOwnership &&
                    RecordFilter::Matches(a_lightingTemplate, profile.ownershipFilter))
                {
                    owner = std::addressof(profile);
                }
            }
            return owner;
        }

        bool TargetsLightingTemplate(
            const ActiveTemplateProfile& a_profile,
            const RE::BGSLightingTemplate* a_lightingTemplate,
            const ActiveTemplateProfile* a_owner)
        {
            return a_profile.appliesStandardSettings &&
                   (!a_owner || Config::IEquals(a_owner->name, a_profile.name)) &&
                   RecordFilter::Matches(a_lightingTemplate, a_profile.filter);
        }

        bool WithinLightingTemplateOwnership(
            const ActiveTemplateProfile& a_profile,
            const ActiveTemplateProfile* a_owner)
        {
            return !a_owner || Config::IEquals(a_owner->name, a_profile.name);
        }

        bool MatchesFilteredLightingTemplateRule(
            const RE::BGSLightingTemplate* a_lightingTemplate,
            const ActiveTemplateProfile::FilteredRule& a_filtered,
            const bool a_matchesProfileFilter)
        {
            if (!a_filtered.rule || !RecordFilter::Matches(a_lightingTemplate, a_filtered.filter))
                return false;
            const auto& rule = *a_filtered.rule;
            const bool hasInclusions = !rule.include.formIDs.empty() ||
                                       !rule.include.contains.empty() ||
                                       !rule.locationTypeInclusions.empty();
            return hasInclusions || rule.ignoreProfileFilters || a_matchesProfileFilter;
        }

        RecordFilter::Resolved ResolveFilteredBaseLightFilter(
            const TuningUtil::FilteredBaseLightRule& a_rule)
        {
            const TuningUtil::PluginFilter noPlugins;
            return RecordFilter::Resolve(a_rule.include, a_rule.exclude, noPlugins, noPlugins);
        }

        bool MatchesFilteredBaseLightRule(
            RE::TESObjectLIGH* a_light,
            const RecordFilter::Resolved& a_filter,
            const TuningUtil::FilteredBaseLightRule& a_rule,
            const Settings& a_settings)
        {
            if (!a_light || !RecordFilter::Matches(a_light, a_filter)) return false;
            if (a_rule.hueFilter.Empty()) return true;
            const auto color = PointLightPatcher::OriginalBaseColor(*a_light);
            return HueFilter::Matches(
                HueMath::Value(color.red / 255.0, color.green / 255.0, color.blue / 255.0),
                a_settings.lightHueRanges,
                a_rule.hueFilter);
        }

        double FilteredBaseLightValue(
            const Settings& a_settings,
            const TuningUtil::FilteredBaseLightRule& a_rule)
        {
            if (const auto exact = a_settings.filteredBaseLightAdjustments.find(a_rule.id);
                exact != a_settings.filteredBaseLightAdjustments.end())
                return exact->second;
            const auto insensitive = std::ranges::find_if(
                a_settings.filteredBaseLightAdjustments,
                [&](const auto& a_entry) { return Config::IEquals(a_entry.first, a_rule.id); });
            return insensitive != a_settings.filteredBaseLightAdjustments.end() ?
                       insensitive->second :
                       a_rule.defaultValue;
        }

        template <class T>
        double* BaseLightHueBand(T& a_settings, const std::string_view a_hue)
        {
            if (a_hue == "red") return &a_settings.red;
            if (a_hue == "orange") return &a_settings.orange;
            if (a_hue == "yellow") return &a_settings.yellow;
            if (a_hue == "green") return &a_settings.green;
            if (a_hue == "teal") return &a_settings.teal;
            if (a_hue == "blue") return &a_settings.blue;
            if (a_hue == "magenta") return &a_settings.magenta;
            return nullptr;
        }

        template <class T>
        const double* BaseLightHueBand(const T& a_settings, const std::string_view a_hue)
        {
            if (a_hue == "red") return &a_settings.red;
            if (a_hue == "orange") return &a_settings.orange;
            if (a_hue == "yellow") return &a_settings.yellow;
            if (a_hue == "green") return &a_settings.green;
            if (a_hue == "teal") return &a_settings.teal;
            if (a_hue == "blue") return &a_settings.blue;
            if (a_hue == "magenta") return &a_settings.magenta;
            return nullptr;
        }

        double* FilteredBaseLightSettingValue(
            PointLightSettings& a_settings,
            const TuningUtil::FilteredBaseLightSetting& a_setting)
        {
            switch (a_setting.operation)
            {
            case TuningUtil::FilteredBaseLightOperation::brightness:
                return &a_settings.fadeMultiplier;
            case TuningUtil::FilteredBaseLightOperation::radius:
                return &a_settings.radiusMultiplier;
            case TuningUtil::FilteredBaseLightOperation::saturation:
                return &a_settings.saturationMultiplier;
            case TuningUtil::FilteredBaseLightOperation::hueScale:
                return a_setting.hue ? BaseLightHueBand(a_settings.hueScales, *a_setting.hue) : nullptr;
            case TuningUtil::FilteredBaseLightOperation::hueShift:
                return a_setting.hue ? BaseLightHueBand(a_settings.hueShift, *a_setting.hue) : nullptr;
            }
            return nullptr;
        }

        bool ApplyFilteredBaseLightSetting(
            PointLightSettings& a_target,
            const double a_value,
            const TuningUtil::FilteredBaseLightSetting& a_setting)
        {
            auto* target = FilteredBaseLightSettingValue(a_target, a_setting);
            if (!target) return false;

            if (a_setting.operation == TuningUtil::FilteredBaseLightOperation::hueShift)
            {
                *target += a_value * a_setting.scale;
                return true;
            }

            const auto multiplier = std::max(0.0, 1.0 + ((a_value - 1.0) * a_setting.scale));
            if (a_setting.operation == TuningUtil::FilteredBaseLightOperation::hueScale) *target = multiplier;
            else *target *= multiplier;
            return true;
        }

        struct FilteredBaseLightResolution
        {
            PointLightPatcher::BaseLightSettingsMap settings;
            std::vector<PointLightPatcher::ReferenceRule> referenceRules;
        };

        FilteredBaseLightResolution ResolveFilteredBaseLightSettings(
            RE::TESDataHandler* a_dataHandler,
            const PointLightSettings& a_baseSettings)
        {
            struct ActiveRule
            {
                std::string profile;
                Settings settings;
                const TuningUtil::FilteredBaseLightRule* rule = nullptr;
                RecordFilter::Resolved filter;
                std::optional<RecordFilter::Resolved> xemiFilter;
            };

            std::vector<ActiveRule> rules;
            for (const auto& discovered : TuningUtil::GetProfiles())
            {
                auto profile = discovered.name;
                const auto& settings = TuningUtil::GetSettings(profile);
                if (!settings.EnableProfile) continue;
                for (const auto& rule : TuningUtil::GetFilteredBaseLightRules(discovered.name))
                {
                    ActiveRule active{
                        .profile = discovered.name,
                        .settings = settings,
                        .rule = std::addressof(rule),
                        .filter = ResolveFilteredBaseLightFilter(rule),
                    };
                    if (rule.useXemiFilter)
                    {
                        const TuningUtil::PluginFilter noPlugins;
                        active.xemiFilter = RecordFilter::Resolve(
                            rule.xemiInclude, rule.xemiExclude, noPlugins, noPlugins);
                        active.xemiFilter->requireIncludedRecordMatch =
                            !rule.xemiInclude.formIDs.empty() || !rule.xemiInclude.contains.empty();
                    }
                    rules.push_back(std::move(active));
                }
            }

            FilteredBaseLightResolution result;
            for (const auto& active : rules)
            {
                if (!active.xemiFilter) continue;
                PointLightSettings adjustment;
                const auto value = FilteredBaseLightValue(active.settings, *active.rule);
                for (const auto& setting : active.rule->settings)
                    ApplyFilteredBaseLightSetting(adjustment, value, setting);
                if (adjustment.fadeMultiplier == 1.0 && adjustment.radiusMultiplier == 1.0) continue;
                PointLightPatcher::ReferenceRule referenceRule{
                    .xemiFilter = *active.xemiFilter,
                    .fadeMultiplier = adjustment.fadeMultiplier,
                    .radiusMultiplier = adjustment.radiusMultiplier,
                };
                for (auto* light : a_dataHandler->GetFormArray<RE::TESObjectLIGH>())
                    if (MatchesFilteredBaseLightRule(light, active.filter, *active.rule, active.settings))
                        referenceRule.baseLights.insert(light->GetFormID());
                if (!referenceRule.baseLights.empty())
                    result.referenceRules.push_back(std::move(referenceRule));
            }
            std::unordered_map<std::string, std::unordered_set<RE::FormID>> profileTargets;
            for (auto* light : a_dataHandler->GetFormArray<RE::TESObjectLIGH>())
            {
                if (!light) continue;
                std::vector<const ActiveRule*> matchingRules;
                for (const auto& active : rules)
                {
                    if (active.xemiFilter ||
                        !MatchesFilteredBaseLightRule(light, active.filter, *active.rule, active.settings)) continue;
                    matchingRules.push_back(std::addressof(active));
                }
                if (matchingRules.empty()) continue;

                auto settings = a_baseSettings;

                for (const auto* active : matchingRules)
                {
                    const auto value = FilteredBaseLightValue(active->settings, *active->rule);
                    const auto hueShift = active->rule->settings.front().operation ==
                                          TuningUtil::FilteredBaseLightOperation::hueShift;
                    const auto neutral = hueShift ? 0.0 : 1.0;
                    if (std::abs(value - neutral) <= 0.0001) continue;
                    for (std::size_t index = 0; index < active->rule->settings.size(); ++index)
                    {
                        ApplyFilteredBaseLightSetting(settings, value, active->rule->settings[index]);
                    }
                }
                if (settings == a_baseSettings) continue;
                result.settings.emplace(light->GetFormID(), std::move(settings));
                for (const auto* active : matchingRules) profileTargets[active->profile].insert(light->GetFormID());
            }
            for (const auto& discovered : TuningUtil::GetProfiles())
            {
                if (!TuningUtil::GetFilteredBaseLightRules(discovered.name).empty())
                {
                    DetailedLogging::Info(
                        "[Base Light] {} | adjusted={}",
                        discovered.name,
                        profileTargets[discovered.name].size());
                }
            }
            DetailedLogging::Info(
                "[Base Light] apply | rules={} | adjusted={}",
                rules.size(),
                result.settings.size());
            return result;
        }
    }  // namespace

    void ApplyAllSettings(const bool a_commitLightPlacer)
    {
        RestoreAllCapturedBaselines();
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("[Lighting] apply failed | TESDataHandler unavailable");
            return;
        }

        const auto activeCellProfiles = GetActiveDirectCellProfiles();
        if (!activeCellProfiles.empty())
        {
            const auto cellCount = ApplyLightingCells(
                TuningUtil::ResolveSettingsStack(activeCellProfiles),
                activeCellProfiles);
            DetailedLogging::Info(
                "[Lighting] cells | profiles={} | targets={}",
                activeCellProfiles.size(),
                cellCount);
        }

        const auto templateProfiles = GetActiveTemplateProfiles();
        std::unordered_map<std::string, std::vector<RE::BGSLightingTemplate*>> templateGroups;
        std::unordered_map<std::string, std::vector<std::string>> groupProfiles;
        std::unordered_map<std::string, FilteredLightingTemplateAdjustments> groupFilteredAdjustments;
        std::unordered_map<std::string, std::size_t> profileTargetCounts;
        for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
        {
            if (!lightingTemplate)
            {
                continue;
            }
            std::string signature;
            std::vector<std::string> matchingProfiles;
            FilteredLightingTemplateAdjustments filteredAdjustments;
            auto hasFilteredMatch = false;
            const auto* owner = LightingTemplateOwner(
                lightingTemplate,
                templateProfiles);
            for (const auto& profile : templateProfiles)
            {
                const auto withinOwnership = WithinLightingTemplateOwnership(
                    profile,
                    owner);
                if (!withinOwnership) continue;
                const auto matchesProfileFilter = RecordFilter::Matches(
                    lightingTemplate,
                    profile.filter);
                if (profile.appliesStandardSettings && matchesProfileFilter)
                {
                    matchingProfiles.push_back(profile.name);
                    signature.append(profile.name).push_back('\x1F');
                    ++profileTargetCounts[profile.name];
                }
                for (const auto& filtered : profile.filteredRules)
                {
                    if (!MatchesFilteredLightingTemplateRule(
                            lightingTemplate,
                            filtered,
                            matchesProfileFilter))
                        continue;
                    AccumulateFilteredLightingTemplateAdjustments(
                        filteredAdjustments,
                        profile.settings,
                        *filtered.rule);
                    signature.append(profile.name)
                        .append("\x1E")
                        .append(filtered.rule->id)
                        .push_back('\x1F');
                    hasFilteredMatch = true;
                }
            }
            if (!matchingProfiles.empty() || hasFilteredMatch)
            {
                templateGroups[signature].push_back(lightingTemplate);
                groupProfiles.try_emplace(signature, std::move(matchingProfiles));
                groupFilteredAdjustments.try_emplace(signature, filteredAdjustments);
            }
        }

        std::size_t templateCount = 0;
        for (const auto& [signature, templates] : templateGroups)
        {
            templateCount += ApplyLightingTemplates(
                TuningUtil::ResolveSettingsStack(groupProfiles[signature]),
                groupProfiles[signature],
                templates,
                groupFilteredAdjustments[signature]);
        }
        for (const auto& profile : templateProfiles)
        {
            DetailedLogging::Info(
                "[Lighting Template] {} | targets={} | scope={}",
                profile.name,
                profileTargetCounts[profile.name],
                profile.declaresOwnership ? "owner" : "shared");
        }
        logger::info(
            "[Lighting Template] apply | stacks={} | targets={}",
            templateGroups.size(),
            templateCount);

        static constexpr std::array pointLightRoots{
            std::string_view{ "pointLights" },
            std::string_view{ "lightHueRanges" },
        };
        std::vector<std::string> pointLightProfiles;
        for (auto profile : TuningUtil::GetProfilesWithSettings(pointLightRoots))
        {
            const auto& settings = TuningUtil::GetSettings(profile);
            if (settings.EnableProfile)
            {
                pointLightProfiles.push_back(std::move(profile));
            }
        }
        const auto pointLightSettings = TuningUtil::ResolveSettingsStack(pointLightProfiles);
        const auto filteredBaseLightSettings = ResolveFilteredBaseLightSettings(
            dataHandler,
            pointLightSettings.pointLights);
        PointLightPatcher::Apply(
            pointLightSettings.pointLights,
            filteredBaseLightSettings.settings,
            filteredBaseLightSettings.referenceRules,
            pointLightSettings.lightHueRanges,
            a_commitLightPlacer);
    }

    bool ProfilesShareLightingTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile)
    {
        if (!UsesTemplateInheritance(a_leftProfile) || !UsesTemplateInheritance(a_rightProfile)) return true;

        const auto templateProfiles = GetActiveTemplateProfiles();
        const auto targetsFor = [&](const std::string_view a_profileName)
        {
            std::unordered_set<RE::FormID> result;
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) return result;
            const auto profile = std::ranges::find_if(
                templateProfiles,
                [&](const ActiveTemplateProfile& a_candidate)
                {
                    return Config::IEquals(a_candidate.name, a_profileName);
                });
            if (profile == templateProfiles.end()) return result;
            for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
            {
                const auto* owner = LightingTemplateOwner(
                    lightingTemplate,
                    templateProfiles);
                if (TargetsLightingTemplate(*profile, lightingTemplate, owner))
                {
                    result.insert(lightingTemplate->GetFormID());
                }
            }
            return result;
        };

        const auto leftTargets = targetsFor(a_leftProfile);
        const auto rightTargets = targetsFor(a_rightProfile);
        return std::ranges::any_of(rightTargets, [&](const auto a_formID)
            { return leftTargets.contains(a_formID); });
    }

    bool ProfilesShareFilteredLightingTemplateTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile,
        const std::string_view a_ruleID)
    {
        const auto templateProfiles = GetActiveTemplateProfiles();
        const auto targetsFor = [&](const std::string_view a_profileName)
        {
            std::unordered_set<RE::FormID> result;
            const auto profile = std::ranges::find_if(
                templateProfiles,
                [&](const ActiveTemplateProfile& a_candidate)
                {
                    return Config::IEquals(a_candidate.name, a_profileName);
                });
            const auto* rule = TuningUtil::FindFilteredLightingTemplateRule(
                std::string(a_profileName),
                a_ruleID);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (profile == templateProfiles.end() || !rule || !dataHandler) return result;

            const auto ruleFilter = ResolveFilteredLightingTemplateFilter(
                *rule,
                a_profileName);
            for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
            {
                const auto* owner = LightingTemplateOwner(
                    lightingTemplate,
                    templateProfiles);
                if (WithinLightingTemplateOwnership(*profile, owner) &&
                    MatchesFilteredLightingTemplateRule(
                        lightingTemplate,
                        { rule, ruleFilter },
                        RecordFilter::Matches(lightingTemplate, profile->filter)))
                {
                    result.insert(lightingTemplate->GetFormID());
                }
            }
            return result;
        };

        const auto leftTargets = targetsFor(a_leftProfile);
        const auto rightTargets = targetsFor(a_rightProfile);
        return std::ranges::any_of(rightTargets, [&](const auto a_formID)
            { return leftTargets.contains(a_formID); });
    }

    bool ProfilesShareFilteredBaseLightTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile,
        const std::string_view a_ruleID)
    {
        const auto targetsFor = [&](const std::string& a_profileName)
        {
            std::unordered_set<RE::FormID> result;
            const auto* rule = TuningUtil::FindFilteredBaseLightRule(a_profileName, a_ruleID);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!rule || !dataHandler || rule->useXemiFilter) return result;
            const auto filter = ResolveFilteredBaseLightFilter(*rule);
            auto profile = a_profileName;
            const auto& settings = TuningUtil::GetSettings(profile);
            for (auto* light : dataHandler->GetFormArray<RE::TESObjectLIGH>())
                if (MatchesFilteredBaseLightRule(light, filter, *rule, settings))
                    result.insert(light->GetFormID());
            return result;
        };

        const auto leftTargets = targetsFor(a_leftProfile);
        const auto rightTargets = targetsFor(a_rightProfile);
        return std::ranges::any_of(rightTargets, [&](const auto a_formID)
            { return leftTargets.contains(a_formID); });
    }

}  // namespace MPL::LightingPatcher
