#pragma once

#include <SliderStorage.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace MPL::SettingsUpdate
{
    enum class Targets : std::uint8_t
    {
        none = 0,
        weather = 1 << 0,
        lighting = 1 << 1,
        pointLights = 1 << 2,
        objects = 1 << 3,
        imageSpaces = 1 << 4,
        all = weather | lighting | pointLights | objects | imageSpaces,
    };

    constexpr Targets operator|(const Targets a_left, const Targets a_right)
    {
        return static_cast<Targets>(static_cast<std::uint8_t>(a_left) | static_cast<std::uint8_t>(a_right));
    }

    constexpr bool Contains(const Targets a_targets, const Targets a_target)
    {
        return (static_cast<std::uint8_t>(a_targets) & static_cast<std::uint8_t>(a_target)) != 0;
    }

    constexpr Targets ForSetting(const std::string_view a_path)
    {
        const auto root = a_path.substr(0, a_path.find('.'));
        if (root == "brightnessMultiplier" || root == "saturationMultiplier" || root == "hueShift" ||
            root == "volumetricLightingIntensityMultiplier" || root == "withinWeatherCompression" ||
            root == "betweenWeatherCompression" || root == "compressionAnchor" || root == "hueScales" ||
            root == "hueRanges" || root == "filteredWeatherAdjustments") return Targets::weather;
        if (root == "lightBrightnessMultiplier" || root == "lightSaturationMultiplier" ||
            root == "lightHueShift" || root == "lightFogPowerMultiplier" || root == "lightFogMaxMultiplier" ||
            root == "filteredLightingTemplateAdjustments") return Targets::lighting;
        if (root == "pointLights" || root == "filteredBaseLightAdjustments") return Targets::pointLights;
        if (root == "objectEffectLighting" || root == "filteredObjectLightingAdjustments") return Targets::objects;
        if (root == "lightImageSpace" || root == "exteriorImageSpace") return Targets::imageSpaces;
        if (root == "lightHueRanges") return Targets::lighting | Targets::pointLights;
        if (a_path == "links.weather" || a_path.starts_with("links.weather.")) return Targets::weather;
        if (a_path == "links.lighting" || a_path.starts_with("links.lighting.")) return Targets::lighting;
        return Targets::all;
    }

    inline Targets ForBinding(const SliderStorage::Binding& a_binding)
    {
        auto targets = Targets::none;
        for (const auto& setting : a_binding.targets) targets = targets | ForSetting(setting.path);
        if (!a_binding.ruleValueMap.empty()) targets = targets | ForSetting(a_binding.ruleValueMap);
        return targets == Targets::none ? Targets::all : targets;
    }

    inline std::string Describe(const Targets a_targets)
    {
        if (a_targets == Targets::all) return "all";
        std::string result;
        const auto append = [&](const Targets a_target, const std::string_view a_name)
        {
            if (!Contains(a_targets, a_target)) return;
            if (!result.empty()) result.push_back(',');
            result.append(a_name);
        };
        append(Targets::weather, "weather");
        append(Targets::lighting, "lighting-templates");
        append(Targets::pointLights, "point-lights");
        append(Targets::objects, "object-lighting");
        append(Targets::imageSpaces, "image-spaces");
        return result.empty() ? "none" : result;
    }
}
