#pragma once

#include <Config/Tuning.h>
#include <cstdint>

namespace MPL::TuningUtil
{
    enum class FilteredWeatherOperation
    {
        brightness,
        saturation,
        hueShift,
    };

    enum class FilteredWeatherDomain
    {
        weather,
        effectLighting,
    };

    struct FilteredWeatherSetting
    {
        FilteredWeatherOperation operation = FilteredWeatherOperation::brightness;
        std::string target;
        std::optional<std::string> hue;
        double scale = 1.0;
        bool ignoreLink = true;

        bool operator==(const FilteredWeatherSetting&) const = default;
    };

    struct FilteredWeatherRule
    {
        std::string id;
        std::string controlID;
        FilteredWeatherDomain domain = FilteredWeatherDomain::weather;
        std::vector<FilteredWeatherSetting> settings;
        std::array<bool, RE::TESWeather::ColorTime::kTotal> times{};
        WeatherFilter include;
        WeatherFilter exclude;
        std::optional<std::string> localLink;
        std::optional<WeatherPatcher::AmbientHueScales> hueScales;
        double defaultValue = 1.0;

        bool operator==(const FilteredWeatherRule&) const = default;
    };

    enum class FilteredLightingTemplateOperation
    {
        brightness,
        fogStrength,
    };

    struct FilteredLightingTemplateSetting
    {
        FilteredLightingTemplateOperation operation = FilteredLightingTemplateOperation::brightness;
        std::string target;
        double scale = 1.0;
        bool ignoreLink = false;

        bool operator==(const FilteredLightingTemplateSetting&) const = default;
    };

    struct FilteredLightingTemplateRule
    {
        std::string id;
        std::string controlID;
        std::vector<FilteredLightingTemplateSetting> settings;
        WeatherFilter include;
        WeatherFilter exclude;
        std::vector<std::string> locationTypeInclusions;
        std::vector<std::string> locationTypeExclusions;
        std::vector<std::string> inclusionMultiLocationExceptions;
        std::vector<std::string> exclusionMultiLocationExceptions;
        double defaultValue = 1.0;

        bool operator==(const FilteredLightingTemplateRule&) const = default;
    };

    enum class FilteredBaseLightOperation
    {
        brightness,
        sunlight,
        saturation,
        hueScale,
        hueShift,
    };

    struct FilteredBaseLightSetting
    {
        FilteredBaseLightOperation operation = FilteredBaseLightOperation::brightness;
        std::optional<std::string> hue;
        double scale = 1.0;

        bool operator==(const FilteredBaseLightSetting&) const = default;
    };

    struct FilteredBaseLightRule
    {
        std::string id;
        std::string controlID;
        std::vector<FilteredBaseLightSetting> settings;
        WeatherFilter include;
        WeatherFilter exclude;
        double defaultValue = 1.0;

        bool operator==(const FilteredBaseLightRule&) const = default;
    };

    struct Profile
    {
        std::string name;
        int priority = 0;
        std::filesystem::path directory;
        std::vector<std::string> defaultSettingRoots;
        std::vector<FilteredWeatherRule> filteredWeatherRules;
        std::vector<FilteredLightingTemplateRule> filteredLightingTemplateRules;
        std::vector<FilteredBaseLightRule> filteredBaseLightRules;
        std::vector<std::string> interiorSliderSettings;
        std::vector<std::string> ignoredInteriorSliderLinks;
    };

    void ApplyDataLoaded();
    void ApplySettings(bool a_commitLightPlacer = true);
    std::uint64_t GetSettingsRevision();
    void InvalidateDiscoveryCaches();
    const std::vector<Profile>& GetProfiles();
    bool IsProfilePluginFiltered(const std::filesystem::path&);
    std::vector<std::string> GetProfilesWithSettings(std::span<const std::string_view>);
    std::optional<std::string> GetOverridingProfile(
        const std::string&,
        std::span<const std::string>);
    int GetProfilePriority(const std::string&);
    std::filesystem::path ProfileDirectory(const std::string&);
    const std::vector<FilteredWeatherRule>& GetFilteredWeatherRules(const std::string&);
    const FilteredWeatherRule* FindFilteredWeatherRule(const std::string&, std::string_view);
    const std::vector<FilteredLightingTemplateRule>& GetFilteredLightingTemplateRules(const std::string&);
    const FilteredLightingTemplateRule* FindFilteredLightingTemplateRule(const std::string&, std::string_view);
    const std::vector<FilteredBaseLightRule>& GetFilteredBaseLightRules(const std::string&);
    const FilteredBaseLightRule* FindFilteredBaseLightRule(const std::string&, std::string_view);
    bool IgnoresInteriorSliderLink(std::span<const std::string>, std::string_view);
    bool ReloadFilteredRules();
    Settings& GetSettings(std::string&);
    Settings ResolveSettingsStack(std::span<const std::string>);
    std::optional<std::string> SerializePresetSettings(std::string&, std::string&);
    bool ApplyPresetPreview(std::string&, std::string_view, std::string_view, std::string&);
    bool ApplyPresetAndRemoveUserOverrides(std::string&, std::string_view, std::string&);
    bool SaveSettings(std::string&);
    bool PromoteUserSettingsToProfile(std::string&, std::string&);
    bool SavePageSettings(std::string&, const std::vector<std::string>&);
    bool RestoreSettings(std::string&);
    bool RestorePageSettings(std::string&, const std::vector<std::string>&);
    bool ResetAllSettingsToDefault(std::string&);
    bool ResetSettingsToDefault(std::string&, const std::vector<std::string>&);
}  // namespace MPL::TuningUtil
