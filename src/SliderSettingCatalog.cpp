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
            NamedPath{ "skyScaleMultiplier", "Sky Scale" }, NamedPath{ "tintStrengthMultiplier", "Tint Strength" },
        };

        bool IEquals(const std::string_view a_left, const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                   std::ranges::equal(a_left, a_right, [](const unsigned char a_lhs, const unsigned char a_rhs)
                   {
                       return std::tolower(a_lhs) == std::tolower(a_rhs);
                   });
        }

        std::string_view ColorTargetLabel(const Entry& a_entry)
        {
            if (!a_entry.linkable) return {};
            const std::span<const NamedPath> colors = a_entry.domain == Domain::weather ?
                std::span<const NamedPath>(weatherColors) : std::span<const NamedPath>(lightingColors);
            const auto color = std::ranges::find(colors, a_entry.target, &NamedPath::key);
            return color == colors.end() ? std::string_view{} : color->label;
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
                    1.0);
        }

        std::vector<Entry> BuildEntries()
        {
            std::vector<Entry> entries;
            entries.reserve(320);

            AddColorCategory(entries, Domain::weather, "Brightness", "brightnessMultiplier",
                weatherColors, FilterOperation::brightness, true, false, 1.0);
            AddColorCategory(entries, Domain::weather, "Saturation", "saturationMultiplier",
                weatherColors, FilterOperation::saturation, true, true, 1.0);
            AddHueShiftCategory(entries, Domain::weather, "Hue Shift", "hueShift",
                weatherColors, FilterOperation::hueShift, true);
            Add(entries, Domain::weather, "Volumetric Lighting", "Intensity",
                "volumetricLightingIntensityMultiplier", {}, {}, FilterOperation::none, false, false, 1.0);
            AddImageSpace(entries, Domain::weather, "Image Space", "exteriorImageSpace");

            AddColorCategory(entries, Domain::lighting, "Brightness", "lightBrightnessMultiplier",
                lightingColors, FilterOperation::brightness, true, false, 1.0);
            AddColorCategory(entries, Domain::lighting, "Saturation", "lightSaturationMultiplier",
                lightingColors, FilterOperation::saturation, true, true, 1.0);
            AddHueShiftCategory(entries, Domain::lighting, "Hue Shift", "lightHueShift",
                lightingColors, FilterOperation::none, true);
            Add(
                entries,
                Domain::lighting,
                "Fog",
                "Fog Power",
                "lightFogPowerMultiplier",
                {},
                {},
                FilterOperation::fogPower,
                false,
                false,
                1.0);
            Add(
                entries,
                Domain::lighting,
                "Fog",
                "Fog Strength",
                "lightFogMaxMultiplier",
                {},
                {},
                FilterOperation::fogStrength,
                false,
                false,
                1.0);
            AddImageSpace(entries, Domain::lighting, "Image Space", "lightImageSpace");
            Add(entries, Domain::lighting, "Point Lights", "Brightness", "pointLights.fadeMultiplier",
                "brightness", {}, FilterOperation::brightness, false, false, 1.0);
            Add(entries, Domain::lighting, "Point Lights", "Radius", "pointLights.radiusMultiplier",
                "radius", {}, FilterOperation::radius, false, false, 1.0);
            Add(entries, Domain::lighting, "Point Lights", "Saturation", "pointLights.saturationMultiplier",
                "saturation", {}, FilterOperation::saturation, false, true, 1.0);
            for (const auto& hue : hues)
            {
                Add(entries, Domain::lighting, "Point Lights", std::string("Hue Shift / ") + std::string(hue.label),
                    "pointLights.hueShift." + std::string(hue.key),
                    "hueShift", hue.key, FilterOperation::hueShift);
            }
            Add(
                entries,
                Domain::lighting,
                "Object Effect Lighting",
                "Emissive Multiplier",
                "objectEffectLighting.emissiveMultiplier",
                "emissiveMultiplier",
                {},
                FilterOperation::brightness,
                false,
                false,
                1.0);
            Add(
                entries,
                Domain::lighting,
                "Object Effect Lighting",
                "Base Color Scale",
                "objectEffectLighting.baseColorScale",
                "baseColorScale",
                {},
                FilterOperation::brightness,
                false,
                false,
                1.0);
            return entries;
        }
    }

    const std::vector<Entry>& Entries()
    {
        static const auto entries = BuildEntries();
        return entries;
    }

    std::string_view SelectionGroup(const Entry& a_entry)
    {
        const auto target = ColorTargetLabel(a_entry);
        return target.empty() ? std::string_view(a_entry.group) : target;
    }

    std::string SelectionLabel(const Entry& a_entry)
    {
        if (ColorTargetLabel(a_entry).empty()) return a_entry.label;
        if (a_entry.hue.empty()) return a_entry.group;
        const auto hue = std::ranges::find(hues, a_entry.hue, &NamedPath::key);
        return a_entry.group + " / " + std::string(hue == hues.end() ? a_entry.hue : hue->label);
    }

    std::vector<std::string_view> Groups(const Domain a_domain)
    {
        std::vector<std::string_view> result;
        for (const auto& entry : Entries())
            if (entry.domain == a_domain && !std::ranges::contains(result, SelectionGroup(entry)))
                result.push_back(SelectionGroup(entry));
        return result;
    }

    std::vector<const Entry*> Entries(const Domain a_domain, const std::string_view a_group)
    {
        std::vector<const Entry*> result;
        for (const auto& entry : Entries())
            if (entry.domain == a_domain && IEquals(SelectionGroup(entry), a_group)) result.push_back(std::addressof(entry));
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
