#pragma once

#include <Config/Weathers.h>

namespace MPL::LightingPatcher
{
    using WeatherPatcher::SettingLink;

    struct InteriorLinks
    {
        SettingLink ambient = std::tuple{ std::string("ambientColors"), 1.0 };
        SettingLink directional = std::tuple{ std::string("ambientColors"), 1.0 };
        SettingLink ambientColors;
        SettingLink fogFar = std::tuple{ std::string("ambientColors"), 1.0 };
        SettingLink fogNear = std::tuple{ std::string("ambientColors"), 1.0 };
    };

    struct InteriorColorSettings
    {
        double ambient = 1.0;
        double directional = 1.0;
        double ambientColors = 1.0;
        double fogFar = 1.0;
        double fogNear = 1.0;
    };

    struct InteriorHueShiftSettings
    {
        WeatherPatcher::HueShiftBands ambient;
        WeatherPatcher::HueShiftBands directional;
        WeatherPatcher::HueShiftBands ambientColors;
        WeatherPatcher::HueShiftBands fogFar;
        WeatherPatcher::HueShiftBands fogNear;
    };

    struct PointLightSettings
    {
        double fadeMultiplier = 1.0;
        double sunlightFadeMultiplier = 1.0;
        double saturationMultiplier = 1.0;
        WeatherPatcher::AmbientHueScales hueScales;
        WeatherPatcher::HueShiftBands hueShift;

        bool operator==(const PointLightSettings&) const = default;
    };

    struct Baseline
    {
        RE::Color ambient;
        RE::Color directional;
        RE::BGSDirectionalAmbientLightingColors ambientColors;
        RE::Color fogFar;
        RE::Color fogNear;
        float fogMax;
    };
}  // namespace MPL::LightingPatcher
