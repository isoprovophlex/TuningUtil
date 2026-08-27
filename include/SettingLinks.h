#pragma once

#include <Config/Lighting.h>
#include <array>
#include <optional>

namespace MPL::WeatherPatcher
{
    struct BrightnessValues
    {
        double ambientMultiplier;
        double sunlightMultiplier;
        double effectLightingMultiplier;
        double fogFarMultiplier;
        double fogNearMultiplier;
        double waterMultiplier;
        double skyStaticsMultiplier;
        double skyUpperMultiplier;
        double skyLowerMultiplier;
        double horizonMultiplier;
        double sunMultiplier;
        double sunGlareMultiplier;
        double moonGlareMultiplier;
        double starsMultiplier;
        double cloudLayers;
    };

    struct CompressionValues
    {
        double ambientCompression;
        double sunlightCompression;
        double effectLightingCompression;
        double fogFarCompression;
        double fogNearCompression;
        double waterMultiplierCompression;
        double skyStaticsCompression;
        double skyUpperCompression;
        double skyLowerCompression;
        double horizonCompression;
        double sunCompression;
        double sunGlareCompression;
        double moonGlareCompression;
        double starsCompression;
    };

    struct SettingLinkResolution
    {
        std::size_t index;
        double scale;
    };

    struct CompressionResolution
    {
        CompressionValues values;
        std::array<std::optional<SettingLinkResolution>, 14> links;
    };

    struct BrightnessResolution
    {
        BrightnessValues values;
        std::array<std::optional<SettingLinkResolution>, 15> links;
    };

    struct SaturationValues
    {
        double ambientMultiplier;
        double sunlightMultiplier;
        double effectLightingMultiplier;
        double fogFarMultiplier;
        double fogNearMultiplier;
        double waterMultiplier;
        double skyStaticsMultiplier;
        double skyUpperMultiplier;
        double skyLowerMultiplier;
        double horizonMultiplier;
        double sunMultiplier;
        double sunGlareMultiplier;
        double moonGlareMultiplier;
        double starsMultiplier;
        double cloudLayers;
        double volumetricLightingMultiplier;
    };

    struct SaturationResolution
    {
        SaturationValues values;
        std::array<std::optional<SettingLinkResolution>, 16> links;
    };

    struct AmbientHueScaleValues
    {
        double red;
        double orange;
        double yellow;
        double green;
        double teal;
        double blue;
        double magenta;
    };

    struct HueShiftResolution
    {
        std::array<AmbientHueScaleValues, 16> values{};
        std::array<std::optional<SettingLinkResolution>, 16> links{};
    };

    struct AnchorValues
    {
        double ambientAnchor;
        double sunlightAnchor;
        double effectLightingAnchor;
        double fogFarAnchor;
        double fogNearAnchor;
        double waterMultiplierAnchor;
        double skyStaticsAnchor;
        double skyUpperAnchor;
        double skyLowerAnchor;
        double horizonAnchor;
        double sunAnchor;
        double sunGlareAnchor;
        double moonGlareAnchor;
        double starsAnchor;
    };

    BrightnessResolution ResolveBrightnessWithLinks(const BrightnessSettings&, const WeatherLinks&);
    CompressionResolution ResolveCompressionWithLinks(const CompressionSettings&, const WeatherLinks&);
    CompressionResolution ResolveWithinWeatherCompressionWithLinks(const CompressionSettings&, const WeatherLinks&);
    SaturationResolution ResolveSaturation(const SaturationSettings&, const WeatherLinks&);
    AmbientHueScaleValues ResolveHueScales(const AmbientHueScales&);
    HueShiftResolution ResolveHueShift(const HueShiftSettings&, const WeatherLinks&);
    AnchorValues ResolveAnchors(const CompressionAnchorSettings&, const WeatherLinks&);
}  // namespace MPL::WeatherPatcher

namespace MPL::LightingPatcher
{
    using InteriorLinkTopology = std::array<std::optional<WeatherPatcher::SettingLinkResolution>, 5>;
    InteriorLinkTopology ResolveInteriorLinks(const InteriorLinks&);
}  // namespace MPL::LightingPatcher
