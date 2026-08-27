#pragma once
#include <Config/Weathers.h>
#include <SettingLinks.h>
#include <TuningUtil.h>
namespace MPL::WeatherPatcher
{
    using Settings = TuningUtil::Settings;
    struct ActivePreset
    {
        std::string category;
        std::string name;
    };

    std::string WeatherName(const RE::TESWeather* a_weather);
    double ColorHueScale(const RE::Color&, const AmbientHueScaleValues&, const HueRanges&);
    double ColorHueScale(const RE::NiColor&, const AmbientHueScaleValues&, const HueRanges&);
    double ColorHueShiftDegrees(const RE::Color&, const HueShiftBands&, const HueRanges&);
    double ColorHueShiftDegrees(const RE::NiColor&, const HueShiftBands&, const HueRanges&);
    std::array<double, 3> RotateHuePreservingSaturationAndLuminance(double, double, double, double);
    void ShiftHue(RE::Color&, double);
    void ShiftHue(RE::NiColor&, double);

    void ApplyAllSettings();
    void ApplyDataLoaded();
    std::optional<double> ReadWeatherAmbientAnchor(std::string_view);
    void ReleaseRuntimeState();
    void InvalidatePresetCache();
    SourceWeatherSet GetSelectableWeathers(std::string&);
    SourceWeatherSet GetFilterableWeathers(std::string&);
    SourceWeatherSet GetFilterableEffectLightingWeathers(std::string&);
    SourceWeatherSet GetFXWeathers();
    bool ProfilesShareWeatherTarget(const std::string&, const std::string&);
    bool ProfilesShareFilteredWeatherTarget(const std::string&, const std::string&, std::string_view);
    bool ProfilesShareEffectLightingTarget(const std::string&, const std::string&);
    DynamicAmbientRange GetDynamicAmbientRange(std::string&, DynamicAmbientMode, DynamicBrightnessField);
    std::optional<DynamicBrightnessStatus> GetDynamicBrightnessStatus(
        std::string&,
        DynamicAmbientMode,
        DynamicBrightnessField);

    std::vector<std::string> GetPresetCategories(std::string&);
    std::vector<std::string> GetPresets(std::string&, const std::string&);
    std::optional<std::string> GetPresetSettings(
        std::string&,
        const std::string&,
        const std::string&,
        std::string&);
    std::vector<ActivePreset> GetActivePresets(std::string&, std::string&);
    std::optional<std::string> GetActivePresetSettings(std::string&, std::string&);
    void DiscardPresetPreview(std::string&);
    void DiscardPresetCatalogChanges(std::string&);
    bool StagePreset(std::string&, const std::string&, const std::string&, std::string&);
    bool UpdatePreset(std::string&, const std::string&, const std::string&, std::string&);
    bool MovePreset(std::string&, const std::string&, const std::string&, int, std::string&);
    bool RenamePreset(
        std::string&,
        const std::string&,
        const std::string&,
        const std::string&,
        std::string&);
    bool RenamePresetCategory(
        std::string&,
        const std::string&,
        const std::string&,
        std::string&);
    bool PreviewPreset(std::string&, const std::string&, const std::string&, std::string&);
    bool PreviewPresetDefault(std::string&, const std::string&, std::string&);
    bool CommitPresetPreviews(std::string&, std::string&);
    bool RemovePresets(
        std::string&,
        const std::vector<std::string>&,
        const std::vector<ActivePreset>&,
        std::string&);

};  // namespace MPL::WeatherPatcher
