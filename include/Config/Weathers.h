#pragma once
#include <string>
#include <tuple>
#include <optional>
#include <vector>
namespace RE
{
    class TESWeather;
}
namespace MPL::WeatherPatcher
{
    constexpr double kDefaultDalcAnchor = 80.0;
    constexpr double kDefaultSunlightAnchor = 255.0;
    constexpr double kDefaultWeatherColorAnchor = 255.0;

    using SettingLink = std::optional<std::tuple<std::string, double>>;

    struct WeatherLinks
    {
        SettingLink ambient;
        SettingLink sunlight;
        SettingLink effectLighting = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink fogFar = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink fogNear = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink water = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink skyStatics = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink skyUpper = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink skyLower = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink horizon = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink sun = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink sunGlare = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink moonGlare = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink stars;
        SettingLink cloudLayers = std::tuple{ std::string("sunlight"), 1.0 };
        SettingLink volumetricLighting = std::tuple{ std::string("sunlight"), 1.0 };
    };

    struct WeatherBaseline
    {
        RE::BGSDirectionalAmbientLightingColors dalc[RE::TESWeather::ColorTime::kTotal];
        RE::Color ambient[RE::TESWeather::ColorTime::kTotal];
        RE::Color sunlight[RE::TESWeather::ColorTime::kTotal];
        RE::Color effectLighting[RE::TESWeather::ColorTime::kTotal];
        RE::Color fogFar[RE::TESWeather::ColorTime::kTotal];
        RE::Color fogNear[RE::TESWeather::ColorTime::kTotal];
        RE::Color waterMultiplier[RE::TESWeather::ColorTime::kTotal];
        RE::Color skyStatics[RE::TESWeather::ColorTime::kTotal];
        RE::Color skyUpper[RE::TESWeather::ColorTime::kTotal];
        RE::Color skyLower[RE::TESWeather::ColorTime::kTotal];
        RE::Color horizon[RE::TESWeather::ColorTime::kTotal];
        RE::Color sun[RE::TESWeather::ColorTime::kTotal];
        RE::Color sunGlare[RE::TESWeather::ColorTime::kTotal];
        RE::Color moonGlare[RE::TESWeather::ColorTime::kTotal];
        RE::Color stars[RE::TESWeather::ColorTime::kTotal];
        RE::Color cloudLayers[RE::TESWeather::kTotalLayers][RE::TESWeather::ColorTime::kTotal];
    };

    struct BrightnessSettings
    {
        double ambient = 1.0;
        double sunlight = 1.0;
        double effectLighting = 1.0;
        double fogFar = 1.0;
        double fogNear = 1.0;
        double water = 1.0;
        double skyStatics = 1.0;
        double skyUpper = 1.0;
        double skyLower = 1.0;
        double horizon = 1.0;
        double sun = 1.0;
        double sunGlare = 1.0;
        double moonGlare = 1.0;
        double stars = 1.0;
        double cloudLayers = 1.0;
    };

    struct CompressionSettings
    {
        double ambient = 0.0;
        double sunlight = 0.0;
        double effectLighting = 0.0;
        double fogFar = 0.0;
        double fogNear = 0.0;
        double water = 0.0;
        double skyStatics = 0.0;
        double skyUpper = 0.0;
        double skyLower = 0.0;
        double horizon = 0.0;
        double sun = 0.0;
        double sunGlare = 0.0;
        double moonGlare = 0.0;
        double stars = 0.0;
    };

    struct SaturationSettings
    {
        double ambient = 1.0;
        double sunlight = 1.0;
        double effectLighting = 1.0;
        double fogFar = 1.0;
        double fogNear = 1.0;
        double water = 1.0;
        double skyStatics = 1.0;
        double skyUpper = 1.0;
        double skyLower = 1.0;
        double horizon = 1.0;
        double sun = 1.0;
        double sunGlare = 1.0;
        double moonGlare = 1.0;
        double stars = 1.0;
        double cloudLayers = 1.0;
        double volumetricLighting = 1.0;
    };

    struct AmbientHueScales
    {
        double red = 1.0;
        double orange = 1.0;
        double yellow = 1.0;
        double green = 1.0;
        double teal = 1.0;
        double blue = 1.0;
        double magenta = 1.0;

        bool operator==(const AmbientHueScales&) const = default;
    };

    struct HueRange
    {
        double start = 0.0;
        double end = 0.0;

        bool operator==(const HueRange&) const = default;
    };

    struct HueRanges
    {
        HueRange red{ 233.75, 10.625 };
        HueRange orange{ 10.625, 31.875 };
        HueRange yellow{ 31.875, 63.75 };
        HueRange green{ 63.75, 106.25 };
        HueRange teal{ 106.25, 148.75 };
        HueRange blue{ 148.75, 191.25 };
        HueRange magenta{ 191.25, 233.75 };

        bool operator==(const HueRanges&) const = default;
    };

    struct HueShiftBands
    {
        double red = 0.0;
        double orange = 0.0;
        double yellow = 0.0;
        double green = 0.0;
        double teal = 0.0;
        double blue = 0.0;
        double magenta = 0.0;

        bool operator==(const HueShiftBands&) const = default;
    };

    struct FXEffectLightingSettings
    {
        double brightnessMultiplier = 1.0;
        double saturationMultiplier = 1.0;
        HueShiftBands hueShift;
    };

    struct HueShiftSettings
    {
        HueShiftBands ambient;
        HueShiftBands sunlight;
        HueShiftBands effectLighting;
        HueShiftBands fogFar;
        HueShiftBands fogNear;
        HueShiftBands water;
        HueShiftBands skyStatics;
        HueShiftBands skyUpper;
        HueShiftBands skyLower;
        HueShiftBands horizon;
        HueShiftBands sun;
        HueShiftBands sunGlare;
        HueShiftBands moonGlare;
        HueShiftBands stars;
        HueShiftBands cloudLayers;
        HueShiftBands volumetricLighting;
    };

    struct CompressionAnchorSettings
    {
        double ambient = kDefaultDalcAnchor;
        double sunlight = kDefaultSunlightAnchor;
        double effectLighting = kDefaultWeatherColorAnchor;
        double fogFar = kDefaultWeatherColorAnchor;
        double fogNear = kDefaultWeatherColorAnchor;
        double water = kDefaultWeatherColorAnchor;
        double skyStatics = kDefaultWeatherColorAnchor;
        double skyUpper = kDefaultWeatherColorAnchor;
        double skyLower = kDefaultWeatherColorAnchor;
        double horizon = kDefaultWeatherColorAnchor;
        double sun = kDefaultWeatherColorAnchor;
        double sunGlare = kDefaultWeatherColorAnchor;
        double moonGlare = kDefaultWeatherColorAnchor;
        double stars = kDefaultWeatherColorAnchor;
    };

    struct DynamicAmbientSettings
    {
        std::optional<double> darkLimit;
        std::optional<double> brightLimit;
    };

    struct DynamicAmbientRange
    {
        double darkLimit = 0.0;
        double brightLimit = 255.0;
        bool available = false;
        const RE::TESWeather* darkWeather = nullptr;
        const RE::TESWeather* brightWeather = nullptr;
    };

    struct DynamicBrightnessStatus
    {
        DynamicAmbientRange source;
        DynamicAmbientRange result;
        std::optional<double> compression;
        bool available = false;
    };

    enum class DynamicAmbientMode
    {
        within,
        between,
    };

    enum class DynamicBrightnessField
    {
        ambient,
        sunlight,
    };

    struct ImageSpaceSettings
    {
        std::optional<bool> ForceCSTonemapping;
        double saturationMultiplier = 1.0;
        double brightnessMultiplier = 1.0;
        double contrastMultiplier = 1.0;
        double sunlightScaleMultiplier = 1.0;
        double skyScaleMultiplier = 1.0;
    };

    typedef std::vector<RE::TESWeather*> SourceWeatherSet;
}  // namespace MPL::WeatherPatcher
