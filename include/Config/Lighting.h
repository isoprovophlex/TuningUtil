#pragma once

#include <Config/Weathers.h>

namespace MPL::LightingPatcher
{
    using WeatherPatcher::SettingLink;

    struct LightingLinks
    {
        SettingLink ambient = std::tuple{ std::string("ambientColors"), 1.0 };
        SettingLink directional = std::tuple{ std::string("ambientColors"), 1.0 };
        SettingLink ambientColors;
        SettingLink fogFar = std::tuple{ std::string("ambientColors"), 1.0 };
        SettingLink fogNear = std::tuple{ std::string("ambientColors"), 1.0 };

        bool operator==(const LightingLinks&) const = default;
    };

    struct LightingColorSettings
    {
        double ambient = 1.0;
        double directional = 1.0;
        double ambientColors = 1.0;
        double fogFar = 1.0;
        double fogNear = 1.0;
    };

    struct LightingHueShiftSettings
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
        double effectFadeMultiplier = 1.0;
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
        float fogPower;
        float fogMax;
    };
}  // namespace MPL::LightingPatcher
