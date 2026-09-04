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

    struct LocationTypeFilter
    {
        std::vector<std::string> locationTypes;
        std::vector<std::string> multiLocationExceptions;

        bool operator==(const LocationTypeFilter&) const = default;
    };

    struct LightingTemplateFilter
    {
        LocationTypeFilter include;
        LocationTypeFilter exclude;

        bool operator==(const LightingTemplateFilter&) const = default;
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
        WeatherFilter weatherInclusions;
        WeatherFilter weatherExclusions;
        PluginFilter weatherPluginOwnership;
        PluginFilter pluginInclusions;
        PluginFilter pluginExclusions;
        std::map<std::string, double> filteredWeatherAdjustments;
        WeatherPatcher::ImageSpaceSettings exteriorImageSpace;
        WeatherFilter effectPointLightInclusions;
        WeatherFilter effectPointLightExclusions;

        LightingPatcher::LightingColorSettings lightBrightnessMultiplier;
        LightingPatcher::LightingColorSettings lightSaturationMultiplier;
        LightingPatcher::LightingHueShiftSettings lightHueShift;
        WeatherPatcher::AmbientHueScales lightAmbientHueScales;
        WeatherPatcher::HueRanges lightHueRanges;
        double lightFogPowerMultiplier = 1.0;
        double lightFogMaxMultiplier = 1.0;
        WeatherPatcher::ImageSpaceSettings lightImageSpace;
        std::vector<std::string> lightingTemplateInclusions;
        std::vector<std::string> lightingTemplateExclusions;
        PluginFilter lightingTemplatePluginOwnership;
        PluginFilter lightingTemplatePluginInclusions;
        PluginFilter lightingTemplatePluginExclusions;
        LightingTemplateFilter lightingTemplateFilter;
        std::map<std::string, double> filteredLightingTemplateAdjustments;
        std::vector<std::string> enableTemplateInherit;
        std::vector<std::string> cellExclusions;
        LightingPatcher::PointLightSettings pointLights;
        std::map<std::string, double> filteredBaseLightAdjustments;
    };
}  // namespace MPL::TuningUtil
