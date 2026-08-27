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
        std::vector<FilteredWeatherSetting> settings;
        std::array<bool, RE::TESWeather::ColorTime::kTotal> times{};
        WeatherFilter include;
        WeatherFilter exclude;
        std::optional<std::string> localLink;
        std::optional<WeatherPatcher::AmbientHueScales> hueScales;
        double defaultValue = 1.0;

        bool operator==(const FilteredWeatherRule&) const = default;
    };

    struct Profile
    {
        std::string name;
        int priority = 0;
        std::filesystem::path directory;
        std::vector<std::string> defaultSettingRoots;
        std::vector<FilteredWeatherRule> filteredWeatherRules;
    };

    void ApplyDataLoaded();
    void ApplySettings(bool a_commitLightPlacer = true);
    std::uint64_t GetSettingsRevision();
    void InvalidateDiscoveryCaches();
    const std::vector<Profile>& GetProfiles();
    std::vector<std::string> GetProfilesWithSettings(std::span<const std::string_view>);
    int GetProfilePriority(const std::string&);
    std::filesystem::path ProfileDirectory(const std::string&);
    const std::vector<FilteredWeatherRule>& GetFilteredWeatherRules(const std::string&);
    const FilteredWeatherRule* FindFilteredWeatherRule(const std::string&, std::string_view);
    bool ReloadFilteredWeatherRules();
    Settings& GetSettings(std::string&);
    Settings ResolveSettingsStack(std::span<const std::string>);
    std::optional<std::string> SerializePresetSettings(std::string&, std::string&);
    bool ApplyPresetPreview(std::string&, std::string_view, std::string_view, std::string&);
    bool ApplyPresetAndRemoveUserOverrides(std::string&, std::string_view, std::string&);
    bool SaveSettings(std::string&);
    bool SavePageSettings(std::string&, const std::vector<std::string>&);
    bool RestoreSettings(std::string&);
    bool RestorePageSettings(std::string&, const std::vector<std::string>&);
    bool ResetAllSettingsToDefault(std::string&);
    bool ResetSettingsToDefault(std::string&, const std::vector<std::string>&);
}  // namespace MPL::TuningUtil
