#pragma once

#include <Config/Lighting.h>
#include <map>

namespace MPL::TuningUtil
{
    struct Links
    {
        WeatherPatcher::WeatherLinks weather;
        LightingPatcher::InteriorLinks interior;
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

    struct Settings
    {
        int profilePriority = 0;
        bool EnableProfile = false;
        bool ShowAdvanced = false;
        Links links;

        WeatherPatcher::BrightnessSettings brightnessMultiplier;
        double volumetricLightingIntensityMultiplier = 1.0;
        WeatherPatcher::SaturationSettings saturationMultiplier;
        WeatherPatcher::AmbientHueScales hueScales;
        WeatherPatcher::HueRanges hueRanges;
        WeatherPatcher::HueShiftSettings hueShift;
        WeatherPatcher::CompressionSettings betweenWeatherCompression;
        WeatherPatcher::CompressionSettings withinWeatherCompression;
        WeatherPatcher::CompressionAnchorSettings compressionAnchor;
        WeatherPatcher::DynamicAmbientSettings dynamicAmbientWithin;
        WeatherPatcher::DynamicAmbientSettings dynamicAmbientBetween;
        WeatherPatcher::DynamicAmbientSettings dynamicSunlightWithin;
        WeatherPatcher::DynamicAmbientSettings dynamicSunlightBetween;
        WeatherFilter weatherInclusions;
        WeatherFilter weatherExclusions;
        PluginFilter pluginInclusions;
        PluginFilter pluginExclusions;
        std::map<std::string, double> filteredWeatherAdjustments;
        WeatherPatcher::ImageSpaceSettings exteriorImageSpace;

        WeatherPatcher::FXEffectLightingSettings fxEffectLighting;
        WeatherFilter effectLightingInclusions;
        WeatherFilter effectLightingExclusions;
        PluginFilter effectLightingPluginInclusions;
        PluginFilter effectLightingPluginExclusions;

        LightingPatcher::InteriorColorSettings intBrightnessMultiplier;
        LightingPatcher::InteriorColorSettings intSaturationMultiplier;
        LightingPatcher::InteriorHueShiftSettings intHueShift;
        WeatherPatcher::AmbientHueScales intAmbientHueScales;
        WeatherPatcher::HueRanges intHueRanges;
        double intFogMaxMultiplier = 1.0;
        WeatherPatcher::ImageSpaceSettings intImageSpace;
        std::vector<std::string> lightingTemplateInclusions;
        std::vector<std::string> lightingTemplateExclusions;
        PluginFilter lightingTemplatePluginInclusions;
        PluginFilter lightingTemplatePluginExclusions;
        std::map<std::string, double> filteredLightingTemplateAdjustments;
        std::vector<std::string> enableTemplateInherit;
        std::vector<std::string> cellExclusions;
        LightingPatcher::PointLightSettings pointLights;
        std::vector<std::string> pointLightEffectLightingExclusions;
    };
}  // namespace MPL::TuningUtil
