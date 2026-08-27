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
            InteriorLinkTopology links{};
        };

        struct InteriorHueShiftResolution
        {
            std::array<WeatherPatcher::HueShiftBands, kFieldCount> values{};
        };

        InteriorLinkTopology ResolveCategoryLinks(
            const InteriorLinks& a_links,
            const std::span<const std::string> a_profileNames,
            const std::string_view a_settingRoot)
        {
            auto result = ResolveInteriorLinks(a_links);
            for (std::size_t index = 0; index < kFieldCount; ++index)
            {
                if (TuningUtil::IgnoresInteriorSliderLink(
                        a_profileNames,
                        std::format("{}.{}", a_settingRoot, kFieldNames[index])))
                    result[index].reset();
            }
            return result;
        }

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

        void BuildStartupFilteredLocationTypeFilters();
        void ApplyStartupCellSettings();

    }  // namespace

    static void CaptureCellBaseline(RE::TESObjectCELL* a_cell)
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
        PointLightPatcher::ReleaseRuntimeState();
    }

    namespace
    {
        struct FilteredLightingTemplateAdjustments
        {
            std::array<double, kFieldCount> brightness{ 1.0, 1.0, 1.0, 1.0, 1.0 };
            double fogStrength = 1.0;
        };

        struct LocationTypeTemplateEvidence
        {
            RE::BGSLightingTemplate* lightingTemplate = nullptr;
            RE::TESObjectCELL* cell = nullptr;
            RE::BGSLocation* location = nullptr;
            RE::BGSKeyword* keyword = nullptr;
            std::size_t matchingCellCount = 1;
        };

        RecordFilter::Resolved ResolveFilteredLightingTemplateFilter(
            const TuningUtil::FilteredLightingTemplateRule& a_rule,
            const std::string_view a_profileName)
        {
            const TuningUtil::PluginFilter noPlugins;
            auto result = RecordFilter::Resolve(a_rule.include, a_rule.exclude, noPlugins, noPlugins);
            const auto key = FilteredLocationTypeFilterKey(a_profileName, a_rule.id);
            if (const auto found = startupFilteredLocationTypeTemplateInclusions.find(key);
                found != startupFilteredLocationTypeTemplateInclusions.end())
            {
                result.includedFormIDs.insert(found->second.begin(), found->second.end());
            }
            if (const auto found = startupFilteredLocationTypeTemplateExclusions.find(
                    key);
                found != startupFilteredLocationTypeTemplateExclusions.end())
            {
                result.excludedFormIDs.insert(found->second.begin(), found->second.end());
            }
            return result;
        }

        std::unordered_set<RE::FormID> BuildLocationTypeTemplateSet(
            const std::string_view a_owner,
            const std::span<const std::string> a_selectors,
            RE::TESDataHandler* a_dataHandler,
            const std::unordered_map<RE::FormID, std::size_t>& a_templateCellCounts,
            const bool a_inclusion)
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
                        "Lighting Template location-type {} for {} ignored invalid keyword selector '{}'",
                        a_inclusion ? "inclusion" : "exclusion",
                        a_owner,
                        selector);
                    continue;
                }
                if (keywordFormIDs.insert(formID).second)
                {
                    keywords.push_back(keyword);
                }
            }

            std::unordered_map<RE::FormID, LocationTypeTemplateEvidence> evidenceByTemplate;
            std::size_t matchingCellCount = 0;
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

                ++matchingCellCount;
                const auto templateFormID = lightingTemplate->GetFormID();
                const auto [evidence, inserted] = evidenceByTemplate.try_emplace(
                    templateFormID,
                    LocationTypeTemplateEvidence{
                        .lightingTemplate = lightingTemplate,
                        .cell = cell,
                        .location = location,
                        .keyword = *matchedKeyword,
                    });
                if (!inserted)
                {
                    ++evidence->second.matchingCellCount;
                }
            }

            std::unordered_set<RE::FormID> result;
            for (const auto& [templateFormID, evidence] : evidenceByTemplate)
            {
                result.insert(templateFormID);
                const auto total = a_templateCellCounts.find(templateFormID);
                const auto totalCellCount = total != a_templateCellCounts.end() ? total->second : 0;
                const auto otherCellCount = totalCellCount > evidence.matchingCellCount ?
                                                totalCellCount - evidence.matchingCellCount :
                                                0;
                logger::info(
                    "Lighting Template location-type {} for {} {} template {} ({}) because cell {} ({}) uses location {} ({}) with keyword {} ({}); matching cells={}, other cells={}",
                    a_inclusion ? "inclusion" : "exclusion",
                    a_owner,
                    a_inclusion ? "included" : "excluded",
                    RecordFilter::FormKey(evidence.lightingTemplate),
                    RecordFilter::DisplayName(evidence.lightingTemplate),
                    RecordFilter::FormKey(evidence.cell),
                    RecordFilter::DisplayName(evidence.cell),
                    RecordFilter::FormKey(evidence.location),
                    RecordFilter::DisplayName(evidence.location),
                    RecordFilter::FormKey(evidence.keyword),
                    RecordFilter::DisplayName(evidence.keyword),
                    evidence.matchingCellCount,
                    otherCellCount);
            }
            logger::info(
                "Lighting Template location-type {} for {} resolved {} keyword(s), matched {} interior CELL record(s), and selected {} template record(s)",
                a_inclusion ? "inclusion" : "exclusion",
                a_owner,
                keywords.size(),
                matchingCellCount,
                result.size());
            return result;
        }

        void BuildStartupFilteredLocationTypeFilters()
        {
            startupFilteredLocationTypeTemplateInclusions.clear();
            startupFilteredLocationTypeTemplateExclusions.clear();
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                logger::warn(
                    "TESDataHandler is unavailable; per-slider Lighting Template location-type filters were not resolved");
                return;
            }

            std::unordered_map<RE::FormID, std::size_t> templateCellCounts;
            for (auto* cell : dataHandler->interiorCells)
            {
                auto* lightingTemplate = cell ? cell->GetRuntimeData().lightingTemplate : nullptr;
                if (lightingTemplate)
                {
                    ++templateCellCounts[lightingTemplate->GetFormID()];
                }
            }

            for (const auto& discovered : TuningUtil::GetProfiles())
            {
                const auto& profileName = discovered.name;
                for (const auto& rule : discovered.filteredLightingTemplateRules)
                {
                    const auto key = FilteredLocationTypeFilterKey(profileName, rule.id);
                    const auto owner = "profile " + profileName + " slider " + rule.id;
                    if (!rule.locationTypeInclusions.empty())
                    {
                        startupFilteredLocationTypeTemplateInclusions.insert_or_assign(
                            key,
                            BuildLocationTypeTemplateSet(
                                owner,
                                rule.locationTypeInclusions,
                                dataHandler,
                                templateCellCounts,
                                true));
                    }
                    if (!rule.locationTypeExclusions.empty())
                    {
                        startupFilteredLocationTypeTemplateExclusions.insert_or_assign(
                            key,
                            BuildLocationTypeTemplateSet(
                                owner,
                                rule.locationTypeExclusions,
                                dataHandler,
                                templateCellCounts,
                                false));
                    }
                }
            }
        }

        std::size_t ApplyLightingTemplates(
            const Settings& a_settings,
            const std::span<const std::string> a_profileNames,
            const std::span<RE::BGSLightingTemplate* const> a_templates,
            const FilteredLightingTemplateAdjustments& a_filtered)
        {
            auto* stat = Config::StatData::GetSingleton();
            const auto brightnessLinks = ResolveCategoryLinks(
                a_settings.links.interior,
                a_profileNames,
                "intBrightnessMultiplier");
            const auto saturationLinks = ResolveCategoryLinks(
                a_settings.links.interior,
                a_profileNames,
                "intSaturationMultiplier");
            const auto hueShiftLinks = ResolveCategoryLinks(
                a_settings.links.interior,
                a_profileNames,
                "intHueShift");
            auto brightness = ResolveCategory(a_settings.intBrightnessMultiplier, brightnessLinks);
            for (std::size_t field = 0; field < brightness.values.size(); ++field)
            {
                brightness.values[field] *= a_filtered.brightness[field];
            }
            const auto saturation = ResolveCategory(a_settings.intSaturationMultiplier, saturationLinks);
            const auto hueShift = ResolveHueShiftCategory(a_settings.intHueShift, hueShiftLinks);
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
                ApplyFogMax(
                    lightingTemplate->data,
                    a_settings.intFogMaxMultiplier * a_filtered.fogStrength,
                    true);
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

        std::size_t ApplyInteriorCells(
            const Settings& a_settings,
            const std::span<const std::string> a_profileNames)
        {
            auto* stat = Config::StatData::GetSingleton();
            const auto brightness = ResolveCategory(
                a_settings.intBrightnessMultiplier,
                ResolveCategoryLinks(a_settings.links.interior, a_profileNames, "intBrightnessMultiplier"));
            const auto saturation = ResolveCategory(
                a_settings.intSaturationMultiplier,
                ResolveCategoryLinks(a_settings.links.interior, a_profileNames, "intSaturationMultiplier"));
            const auto hueShift = ResolveHueShiftCategory(
                a_settings.intHueShift,
                ResolveCategoryLinks(a_settings.links.interior, a_profileNames, "intHueShift"));
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
                        "TuningUtil profile {} ignored unknown template inheritance flag {}",
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
                result.push_back({
                    .name = std::move(profile),
                    .flags = flags,
                    .excludedCellFormIDs = ResolveConfiguredFormIDs(settings.cellExclusions),
                    .lightingTemplateFilter = RecordFilter::Resolve(
                        settings.lightingTemplateInclusions,
                        settings.lightingTemplateExclusions,
                        settings.lightingTemplatePluginInclusions,
                        settings.lightingTemplatePluginExclusions),
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
                "Template inheritance for profile {} excluded cell {:08X};{} through cellExclusions",
                a_profile.name,
                a_cell->GetFormID(),
                RecordFilter::DisplayName(a_cell));
        }

        bool UsesTemplateInheritance(const std::string_view a_profileName)
        {
            return std::ranges::any_of(startupTemplateDrivenProfiles, [&](const auto& a_profile)
                { return Config::IEquals(a_profileName, a_profile); });
        }

        std::vector<std::string> GetActiveDirectCellProfiles()
        {
            static constexpr std::array roots{
                std::string_view{ "intBrightnessMultiplier" },
                std::string_view{ "intSaturationMultiplier" },
                std::string_view{ "intHueShift" },
                std::string_view{ "intAmbientHueScales" },
                std::string_view{ "intHueRanges" },
                std::string_view{ "intFogMaxMultiplier" },
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
            auto* interior = a_cell->GetRuntimeData().cellData.interior;
            auto* lightingTemplate = a_cell->GetRuntimeData().lightingTemplate;
            if (!interior || !lightingTemplate)
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
                interior->lightingTemplateInheritanceFlags |= profile.flags;
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
                logger::warn("TESDataHandler is unavailable; startup CELL settings were not applied");
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
                    "Startup template inheritance for profile {} matched {} interior CELL record(s)",
                    profile.name,
                    inheritTargetCounts[profile.name]);
            }
        }

        struct ActiveTemplateProfile
        {
            std::string name;
            Settings settings;
            RecordFilter::Resolved filter;
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
            for (const auto& setting : a_rule.settings)
            {
                const auto multiplier = std::max(0.0, 1.0 + ((value - 1.0) * setting.scale));
                if (setting.operation == TuningUtil::FilteredLightingTemplateOperation::fogStrength)
                {
                    a_adjustments.fogStrength *= multiplier;
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
            auto directProfiles = TuningUtil::GetProfilesWithSettings(roots);
            for (const auto& discovered : TuningUtil::GetProfiles())
            {
                const auto ownsDirectSettings = std::ranges::any_of(directProfiles, [&](const auto& a_profile)
                    { return Config::IEquals(a_profile, discovered.name); });
                const auto& filteredRules = TuningUtil::GetFilteredLightingTemplateRules(discovered.name);
                if (!ownsDirectSettings && filteredRules.empty()) continue;

                auto profile = discovered.name;
                const auto& settings = TuningUtil::GetSettings(profile);
                if (!settings.EnableProfile)
                {
                    continue;
                }
                ActiveTemplateProfile active{
                    .name = std::move(profile),
                    .settings = settings,
                    .filter = RecordFilter::Resolve(
                        settings.lightingTemplateInclusions,
                        settings.lightingTemplateExclusions,
                        settings.lightingTemplatePluginInclusions,
                        settings.lightingTemplatePluginExclusions),
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

        const auto activeCellProfiles = GetActiveDirectCellProfiles();
        if (!activeCellProfiles.empty())
        {
            const auto cellCount = ApplyInteriorCells(
                TuningUtil::ResolveSettingsStack(activeCellProfiles),
                activeCellProfiles);
            DetailedLogging::Info(
                "Applied {} stacked Lighting profile(s) to {} direct interior cell record(s)",
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
            for (const auto& profile : templateProfiles)
            {
                if (RecordFilter::Matches(lightingTemplate, profile.filter))
                {
                    matchingProfiles.push_back(profile.name);
                    signature.append(profile.name).push_back('\x1F');
                    ++profileTargetCounts[profile.name];
                    for (const auto& filtered : profile.filteredRules)
                    {
                        if (!filtered.rule || !RecordFilter::Matches(lightingTemplate, filtered.filter)) continue;
                        AccumulateFilteredLightingTemplateAdjustments(
                            filteredAdjustments,
                            profile.settings,
                            *filtered.rule);
                        signature.append(profile.name)
                            .append("\x1E")
                            .append(filtered.rule->id)
                            .push_back('\x1F');
                    }
                }
            }
            if (!matchingProfiles.empty())
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
            std::string_view{ "pointLightEffectLightingExclusions" },
        };
        std::vector<std::string> pointLightProfiles;
        std::vector<std::string> pointLightRegionExclusions;
        for (auto profile : TuningUtil::GetProfilesWithSettings(pointLightRoots))
        {
            const auto& settings = TuningUtil::GetSettings(profile);
            if (settings.EnableProfile)
            {
                for (const auto& configured : settings.pointLightEffectLightingExclusions)
                {
                    if (std::ranges::none_of(
                            pointLightRegionExclusions,
                            [&](const std::string& a_existing)
                            {
                                return Config::IEquals(a_existing, configured);
                            }))
                    {
                        pointLightRegionExclusions.push_back(configured);
                    }
                }
                pointLightProfiles.push_back(std::move(profile));
            }
        }
        const auto pointLightSettings = TuningUtil::ResolveSettingsStack(pointLightProfiles);
        PointLightPatcher::Apply(
            pointLightSettings.pointLights,
            pointLightSettings.intHueRanges,
            pointLightRegionExclusions,
            a_commitLightPlacer);
    }

    bool ProfilesShareInteriorTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile)
    {
        if (!UsesTemplateInheritance(a_leftProfile) || !UsesTemplateInheritance(a_rightProfile)) return true;

        const auto targetsFor = [](std::string a_profileName)
        {
            std::unordered_set<RE::FormID> result;
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) return result;
            const auto& settings = TuningUtil::GetSettings(a_profileName);
            const auto filter = RecordFilter::Resolve(
                settings.lightingTemplateInclusions,
                settings.lightingTemplateExclusions,
                settings.lightingTemplatePluginInclusions,
                settings.lightingTemplatePluginExclusions);
            for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
            {
                if (RecordFilter::Matches(lightingTemplate, filter))
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
        const auto targetsFor = [&](std::string a_profileName)
        {
            std::unordered_set<RE::FormID> result;
            const auto* rule = TuningUtil::FindFilteredLightingTemplateRule(a_profileName, a_ruleID);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!rule || !dataHandler) return result;

            const auto& settings = TuningUtil::GetSettings(a_profileName);
            const auto profileFilter = RecordFilter::Resolve(
                settings.lightingTemplateInclusions,
                settings.lightingTemplateExclusions,
                settings.lightingTemplatePluginInclusions,
                settings.lightingTemplatePluginExclusions);
            const auto ruleFilter = ResolveFilteredLightingTemplateFilter(*rule, a_profileName);
            for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
            {
                if (RecordFilter::Matches(lightingTemplate, profileFilter) &&
                    RecordFilter::Matches(lightingTemplate, ruleFilter))
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

}  // namespace MPL::LightingPatcher
