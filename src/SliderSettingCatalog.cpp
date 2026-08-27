#include <SliderSettingCatalog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <ranges>
#include <span>

namespace MPL::SliderSettingCatalog
{
    namespace
    {
        struct NamedPath
        {
            std::string_view key;
            std::string_view label;
        };

        constexpr std::array weatherColors{
            NamedPath{ "sunlight", "Sunlight" }, NamedPath{ "ambient", "Ambient" },
            NamedPath{ "effectLighting", "Effect Lighting" }, NamedPath{ "fogFar", "Fog Far" },
            NamedPath{ "fogNear", "Fog Near" }, NamedPath{ "water", "Water" },
            NamedPath{ "skyStatics", "Sky Statics" }, NamedPath{ "skyUpper", "Sky Upper" },
            NamedPath{ "skyLower", "Sky Lower" }, NamedPath{ "horizon", "Horizon" },
            NamedPath{ "sun", "Sun" }, NamedPath{ "sunGlare", "Sun Glare" },
            NamedPath{ "moonGlare", "Moon Glare" }, NamedPath{ "stars", "Stars" },
            NamedPath{ "cloudLayers", "Cloud Layers" }, NamedPath{ "volumetricLighting", "Volumetric Lighting" },
        };
        constexpr std::array compressionColors{
            weatherColors[0], weatherColors[1], weatherColors[2], weatherColors[3], weatherColors[4],
            weatherColors[5], weatherColors[6], weatherColors[7], weatherColors[8], weatherColors[9],
            weatherColors[10], weatherColors[11], weatherColors[12], weatherColors[13],
        };
        constexpr std::array lightingColors{
            NamedPath{ "ambientColors", "Ambient Colors (DALC)" }, NamedPath{ "ambient", "Ambient" },
            NamedPath{ "directional", "Directional" }, NamedPath{ "fogFar", "Fog Far" },
            NamedPath{ "fogNear", "Fog Near" },
        };
        constexpr std::array hues{
            NamedPath{ "red", "Red" }, NamedPath{ "orange", "Orange" }, NamedPath{ "yellow", "Yellow" },
            NamedPath{ "green", "Green" }, NamedPath{ "teal", "Teal" }, NamedPath{ "blue", "Blue" },
            NamedPath{ "magenta", "Magenta" },
        };
        constexpr std::array imageSpaceValues{
            NamedPath{ "saturationMultiplier", "Saturation" }, NamedPath{ "brightnessMultiplier", "Brightness" },
            NamedPath{ "contrastMultiplier", "Contrast" }, NamedPath{ "sunlightScaleMultiplier", "Sunlight Scale" },
            NamedPath{ "skyScaleMultiplier", "Sky Scale" },
        };

        bool IEquals(const std::string_view a_left, const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                   std::ranges::equal(a_left, a_right, [](const unsigned char a_lhs, const unsigned char a_rhs)
                   {
                       return std::tolower(a_lhs) == std::tolower(a_rhs);
                   });
        }

        void Add(
            std::vector<Entry>& a_entries,
            const Domain a_domain,
            const std::string_view a_group,
            const std::string_view a_label,
            std::string a_path,
            const std::string_view a_target = {},
            const std::string_view a_hue = {},
            const FilterOperation a_filterOperation = FilterOperation::none,
            const bool a_linkable = false,
            const bool a_hueScales = false,
            const bool a_aggregate = false,
            const std::optional<double> a_neutralValue = std::nullopt)
        {
            a_entries.push_back({
                a_domain,
                std::string(a_group),
                std::string(a_label),
                std::move(a_path),
                std::string(a_target),
                std::string(a_hue),
                a_filterOperation,
                a_linkable,
                a_hueScales,
                a_aggregate,
                a_neutralValue,
            });
        }

        void AddColorCategory(
            std::vector<Entry>& a_entries,
            const Domain a_domain,
            const std::string_view a_group,
            const std::string_view a_prefix,
            const std::span<const NamedPath> a_colors,
            const FilterOperation a_operation,
            const bool a_linkable,
            const bool a_hueScales = false,
            const std::optional<double> a_neutralValue = std::nullopt)
        {
            for (const auto& color : a_colors)
                Add(
                    a_entries,
                    a_domain,
                    a_group,
                    color.label,
                    std::string(a_prefix) + "." + std::string(color.key),
                    color.key,
                    {},
                    a_operation,
                    a_linkable,
                    a_hueScales,
                    false,
                    a_neutralValue);
        }

        void AddHueShiftCategory(
            std::vector<Entry>& a_entries,
            const Domain a_domain,
            const std::string_view a_group,
            const std::string_view a_prefix,
            const std::span<const NamedPath> a_colors,
            const FilterOperation a_operation,
            const bool a_linkable)
        {
            for (const auto& color : a_colors)
            {
                Add(
                    a_entries,
                    a_domain,
                    a_group,
                    std::string(color.label) + " / All Hues",
                    std::string(a_prefix) + "." + std::string(color.key),
                    color.key,
                    {},
                    a_operation,
                    a_linkable,
                    false,
                    true);
                for (const auto& hue : hues)
                    Add(
                        a_entries,
                        a_domain,
                        a_group,
                        std::string(color.label) + " / " + std::string(hue.label),
                        std::string(a_prefix) + "." + std::string(color.key) + "." + std::string(hue.key),
                        color.key,
                        hue.key,
                        a_operation,
                        a_linkable);
            }
        }

        void AddHueValues(
            std::vector<Entry>& a_entries,
            const Domain a_domain,
            const std::string_view a_group,
            const std::string_view a_prefix,
            const std::optional<double> a_neutralValue = std::nullopt)
        {
            for (const auto& hue : hues)
                Add(
                    a_entries,
                    a_domain,
                    a_group,
                    hue.label,
                    std::string(a_prefix) + "." + std::string(hue.key),
                    {},
                    {},
                    FilterOperation::none,
                    false,
                    false,
                    false,
                    a_neutralValue);
        }

        void AddHueRanges(
            std::vector<Entry>& a_entries,
            const Domain a_domain,
            const std::string_view a_group,
            const std::string_view a_prefix)
        {
            for (const auto& hue : hues)
            {
                Add(a_entries, a_domain, a_group, std::string(hue.label) + " Start",
                    std::string(a_prefix) + "." + std::string(hue.key) + ".start");
                Add(a_entries, a_domain, a_group, std::string(hue.label) + " End",
                    std::string(a_prefix) + "." + std::string(hue.key) + ".end");
            }
        }

        void AddImageSpace(
            std::vector<Entry>& a_entries,
            const Domain a_domain,
            const std::string_view a_group,
            const std::string_view a_prefix)
        {
            for (const auto& value : imageSpaceValues)
                Add(a_entries, a_domain, a_group, value.label,
                    std::string(a_prefix) + "." + std::string(value.key),
                    {},
                    {},
                    FilterOperation::none,
                    false,
                    false,
                    false,
                    1.0);
        }

        std::vector<Entry> BuildEntries()
        {
            std::vector<Entry> entries;
            entries.reserve(320);

            AddColorCategory(entries, Domain::weather, "Brightness", "brightnessMultiplier",
                std::span(weatherColors).first(15), FilterOperation::brightness, true, false, 1.0);
            AddColorCategory(entries, Domain::weather, "Saturation", "saturationMultiplier",
                weatherColors, FilterOperation::saturation, true, true, 1.0);
            AddHueShiftCategory(entries, Domain::weather, "Hue Shift", "hueShift",
                weatherColors, FilterOperation::hueShift, true);
            AddColorCategory(entries, Domain::weather, "Between Weather Compression", "betweenWeatherCompression",
                compressionColors, FilterOperation::none, true);
            AddColorCategory(entries, Domain::weather, "Within Weather Compression", "withinWeatherCompression",
                compressionColors, FilterOperation::none, true);
            AddColorCategory(entries, Domain::weather, "Compression Anchors", "compressionAnchor",
                compressionColors, FilterOperation::none, true);
            AddHueValues(entries, Domain::weather, "Saturation Scales", "hueScales", 1.0);
            AddHueRanges(entries, Domain::weather, "Hue Ranges", "hueRanges");
            Add(entries, Domain::weather, "Volumetric Lighting", "Intensity",
                "volumetricLightingIntensityMultiplier", {}, {}, FilterOperation::none, false, false, false, 1.0);
            AddImageSpace(entries, Domain::weather, "Image Space", "exteriorImageSpace");

            AddColorCategory(entries, Domain::lighting, "Brightness", "intBrightnessMultiplier",
                lightingColors, FilterOperation::brightness, true, false, 1.0);
            AddColorCategory(entries, Domain::lighting, "Saturation", "intSaturationMultiplier",
                lightingColors, FilterOperation::none, true, false, 1.0);
            AddHueShiftCategory(entries, Domain::lighting, "Hue Shift", "intHueShift",
                lightingColors, FilterOperation::none, true);
            AddHueValues(entries, Domain::lighting, "Saturation Scales", "intAmbientHueScales", 1.0);
            AddHueRanges(entries, Domain::lighting, "Hue Ranges", "intHueRanges");
            Add(
                entries,
                Domain::lighting,
                "Fog",
                "Fog Strength",
                "intFogMaxMultiplier",
                {},
                {},
                FilterOperation::fogStrength,
                false,
                false,
                false,
                1.0);
            AddImageSpace(entries, Domain::lighting, "Image Space", "intImageSpace");
            Add(entries, Domain::lighting, "Effect Lighting", "Brightness",
                "fxEffectLighting.brightnessMultiplier", "effectLighting", {}, FilterOperation::brightness, false, false, false, 1.0);
            Add(entries, Domain::lighting, "Effect Lighting", "Saturation",
                "fxEffectLighting.saturationMultiplier", "effectLighting", {}, FilterOperation::saturation, false, false, false, 1.0);
            for (const auto& hue : hues)
                Add(entries, Domain::lighting, "Effect Lighting", std::string("Hue Shift / ") + std::string(hue.label),
                    "fxEffectLighting.hueShift." + std::string(hue.key),
                    "effectLighting", hue.key, FilterOperation::hueShift);
            Add(entries, Domain::lighting, "Point Lights", "Brightness", "pointLights.fadeMultiplier",
                "brightness", {}, FilterOperation::brightness, false, false, false, 1.0);
            Add(entries, Domain::lighting, "Point Lights", "Sunlight", "pointLights.sunlightFadeMultiplier",
                "sunlight", {}, FilterOperation::brightness, false, false, false, 1.0);
            Add(entries, Domain::lighting, "Point Lights", "Saturation", "pointLights.saturationMultiplier",
                "saturation", {}, FilterOperation::saturation, false, false, false, 1.0);
            for (const auto& hue : hues)
            {
                Add(entries, Domain::lighting, "Point Lights", std::string("Saturation Scale / ") + std::string(hue.label),
                    "pointLights.hueScales." + std::string(hue.key),
                    "hueScale", hue.key, FilterOperation::saturation, false, false, false, 1.0);
                Add(entries, Domain::lighting, "Point Lights", std::string("Hue Shift / ") + std::string(hue.label),
                    "pointLights.hueShift." + std::string(hue.key),
                    "hueShift", hue.key, FilterOperation::hueShift);
            }
            return entries;
        }
    }

    const std::vector<Entry>& Entries()
    {
        static const auto entries = BuildEntries();
        return entries;
    }

    std::vector<std::string_view> Groups(const Domain a_domain)
    {
        std::vector<std::string_view> result;
        for (const auto& entry : Entries())
            if (entry.domain == a_domain && !std::ranges::contains(result, std::string_view(entry.group)))
                result.push_back(entry.group);
        return result;
    }

    std::vector<const Entry*> Entries(const Domain a_domain, const std::string_view a_group)
    {
        std::vector<const Entry*> result;
        for (const auto& entry : Entries())
            if (entry.domain == a_domain && IEquals(entry.group, a_group)) result.push_back(std::addressof(entry));
        return result;
    }

    const Entry* Find(const std::string_view a_path)
    {
        const auto found = std::ranges::find_if(Entries(), [&](const Entry& a_entry)
            { return IEquals(a_entry.path, a_path); });
        return found == Entries().end() ? nullptr : std::addressof(*found);
    }

    std::optional<double> NeutralValue(const std::string_view a_path)
    {
        if (const auto* entry = Find(a_path)) return entry->neutralValue;

        const auto prefix = std::string(a_path) + ".";
        std::optional<double> result;
        auto found = false;
        for (const auto& entry : Entries())
        {
            if (entry.path.size() <= prefix.size() ||
                !IEquals(std::string_view(entry.path).substr(0, prefix.size()), prefix))
                continue;
            if (!entry.neutralValue || (result && *result != *entry.neutralValue)) return std::nullopt;
            result = entry.neutralValue;
            found = true;
        }
        return found ? result : std::nullopt;
    }

    bool IsFilteredOperation(const FilterOperation a_operation)
    {
        return a_operation != FilterOperation::none;
    }
}  // namespace MPL::SliderSettingCatalog
