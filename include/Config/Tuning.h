#pragma once

#include <Config/Lighting.h>
#include <map>

namespace MPL::TuningUtil
{
    struct Links
    {
        WeatherPatcher::WeatherLinks weather;
        LightingPatcher::LightingLinks lighting;
    };

    struct WeatherFilter
    {
        std::vector<std::string> formIDs;
        std::vector<std::string> contains;

        bool operator==(const WeatherFilter&) const = default;
    };

    struct PluginFilter
    {
        std::vector<std::string> exact;
        std::vector<std::string> contains;

        bool operator==(const PluginFilter&) const = default;
    };

    struct ExclusiveControl
    {
        bool weather = false;
        bool exteriorImageSpace = false;
        bool lightingTemplates = false;
        bool cellLighting = false;
        bool lightingImageSpace = false;

        bool operator==(const ExclusiveControl&) const = default;
    };

    struct Settings
    {
        int profilePriority = 0;
        bool EnableProfile = true;
        bool ShowAdvanced = false;
        ExclusiveControl exclusiveControl;
        Links links;
        std::map<std::string, double> sliderValues;

        WeatherPatcher::BrightnessSettings brightnessMultiplier;
        double volumetricLightingIntensityMultiplier = 1.0;
        WeatherPatcher::SaturationSettings saturationMultiplier;
        WeatherPatcher::AmbientHueScales hueScales;
        WeatherPatcher::HueRanges hueRanges;
        WeatherPatcher::HueShiftSettings hueShift;
        WeatherPatcher::CompressionSettings betweenWeatherCompression;
        WeatherPatcher::CompressionSettings withinWeatherCompression;
        WeatherPatcher::CompressionAnchorSettings compressionAnchor;
        WeatherFilter weatherInclusions;
        WeatherFilter weatherExclusions;
        PluginFilter weatherPluginOwnership;
        PluginFilter weatherPluginInclusions;
        PluginFilter weatherPluginExclusions;
        std::map<std::string, double> filteredWeatherAdjustments;
        WeatherPatcher::ImageSpaceSettings exteriorImageSpace;

        LightingPatcher::LightingColorSettings lightBrightnessMultiplier;
#ifdef TUNING_ATLM
        std::optional<LightingPatcher::LightingLuminanceSettings> normalizeLuminance{ std::in_place };
#else
        std::optional<LightingPatcher::LightingLuminanceSettings> normalizeLuminance;
#endif
        LightingPatcher::LightingColorSettings lightSaturationMultiplier;
        LightingPatcher::LightingHueShiftSettings lightHueShift;
        WeatherPatcher::HueRanges lightHueRanges;
        double lightFogPowerMultiplier = 1.0;
        double lightFogMaxMultiplier = 1.0;
        WeatherPatcher::ImageSpaceSettings lightImageSpace;
        std::vector<std::string> lightingTemplateInclusions;
        std::vector<std::string> lightingTemplateExclusions;
        PluginFilter lightingTemplatePluginOwnership;
        PluginFilter lightingTemplatePluginInclusions;
        PluginFilter lightingTemplatePluginExclusions;
        std::map<std::string, double> filteredLightingTemplateAdjustments;
        std::vector<std::string> enableTemplateInherit;
        std::vector<std::string> cellExclusions;
        LightingPatcher::PointLightSettings pointLights;
        std::map<std::string, double> filteredBaseLightAdjustments;
        std::map<std::string, double> filteredObjectLightingAdjustments;
    };
}  // namespace MPL::TuningUtil
