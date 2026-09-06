#pragma once

#include <Config/Tuning.h>
#include <HueFilter.h>
#include <ProfileSetup.h>
#include <SliderStorage.h>
#include <cstdint>
#include <map>

namespace MPL::SliderCreator
{
    struct Definition;
}

namespace MPL::TuningUtil
{
    using PresetSelections = std::map<std::string, std::string>;

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

        bool operator==(const FilteredWeatherSetting&) const = default;
    };

    struct FilteredWeatherRule
    {
        std::string id;
        std::string controlID;
        std::vector<FilteredWeatherSetting> settings;
        std::optional<WeatherPatcher::WeatherLinks> customLinks;
        std::array<bool, RE::TESWeather::ColorTime::kTotal> times{};
        WeatherFilter include;
        WeatherFilter exclude;
        bool ignoreProfileFilters = false;
        std::optional<WeatherPatcher::AmbientHueScales> hueScales;
        double defaultValue = 1.0;

        bool operator==(const FilteredWeatherRule&) const = default;
    };

    enum class FilteredLightingTemplateOperation
    {
        brightness,
        fogPower,
        fogStrength,
    };

    struct FilteredLightingTemplateSetting
    {
        FilteredLightingTemplateOperation operation = FilteredLightingTemplateOperation::brightness;
        std::string target;
        double scale = 1.0;

        bool operator==(const FilteredLightingTemplateSetting&) const = default;
    };

    struct FilteredLightingTemplateRule
    {
        std::string id;
        std::string controlID;
        std::vector<FilteredLightingTemplateSetting> settings;
        std::optional<LightingPatcher::LightingLinks> customLinks;
        WeatherFilter include;
        WeatherFilter exclude;
        std::vector<std::string> locationTypeInclusions;
        std::vector<std::string> locationTypeExclusions;
        std::vector<std::string> inclusionMultiLocationExceptions;
        std::vector<std::string> exclusionMultiLocationExceptions;
        bool ignoreProfileFilters = false;
        double defaultValue = 1.0;

        bool operator==(const FilteredLightingTemplateRule&) const = default;
    };

    enum class FilteredBaseLightOperation
    {
        brightness,
        radius,
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
        HueFilter::Selection hueFilter;
        bool useXemiFilter = false;
        WeatherFilter xemiInclude;
        WeatherFilter xemiExclude;
        double defaultValue = 1.0;

        bool operator==(const FilteredBaseLightRule&) const = default;
    };

    enum class FilteredObjectLightingOperation
    {
        emissiveMultiplier,
        baseColorScale,
    };

    struct FilteredObjectLightingSetting
    {
        FilteredObjectLightingOperation operation = FilteredObjectLightingOperation::emissiveMultiplier;
        double scale = 1.0;

        bool operator==(const FilteredObjectLightingSetting&) const = default;
    };

    struct FilteredObjectLightingRule
    {
        std::string id;
        std::string controlID;
        std::vector<FilteredObjectLightingSetting> settings;
        WeatherFilter include;
        WeatherFilter exclude;
        WeatherFilter xemiInclude;
        WeatherFilter xemiExclude;
        double defaultValue = 1.0;

        bool operator==(const FilteredObjectLightingRule&) const = default;
    };

    struct Profile
    {
        std::string name;
        int priority = 0;
        std::filesystem::path directory;
        std::string ambientAnchorWeather{ "SkyrimClear" };
        std::optional<WeatherPatcher::WeatherCompressionAnchors> runtimeCompressionAnchors;
        std::vector<std::string> disabledProfiles;
        std::vector<std::string> defaultSettingRoots;
        std::vector<SliderStorage::Binding> sliderBindings;
        std::vector<FilteredWeatherRule> filteredWeatherRules;
        std::vector<FilteredLightingTemplateRule> filteredLightingTemplateRules;
        std::vector<FilteredBaseLightRule> filteredBaseLightRules;
        std::vector<FilteredObjectLightingRule> filteredObjectLightingRules;
        std::vector<std::string> lightingSliderSettings;
        std::map<std::string, LightingPatcher::LightingLinks, std::less<>> customLightingSliderLinks;
        std::vector<std::string> lightingMenuSettings;
        std::vector<std::string> weatherMenuSettings;
    };

    void ApplyDataLoaded();
    void ApplySettings(bool a_commitLightPlacer = true);
    void BeginSliderValueEdit();
    void EndSliderValueEdit(bool a_changed);
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
    const std::vector<FilteredObjectLightingRule>& GetFilteredObjectLightingRules(const std::string&);
    const FilteredObjectLightingRule* FindFilteredObjectLightingRule(const std::string&, std::string_view);
    LightingPatcher::LightingLinks ResolveLightingSliderLinks(
        std::span<const std::string>,
        std::string_view,
        const LightingPatcher::LightingLinks&);
    std::map<std::string, LightingPatcher::LightingLinks, std::less<>> ResolveLightingSliderLinkOverrides(
        std::span<const std::string>,
        std::string_view);
    bool ReloadFilteredRules(bool a_force = false);
    bool SetSliderCreatorPreview(
        std::string&,
        std::string_view,
        const SliderCreator::Definition&,
        double,
        bool&,
        std::string&);
    Settings& GetSettings(std::string&);
    Settings ResolveSettingsStack(std::span<const std::string>);
    WeatherPatcher::WeatherCompressionAnchors ResolveCompressionAnchors(std::span<const std::string>);
    std::optional<std::string> SerializePresetSettings(std::string&, std::string&);
    std::optional<std::string> ResolvePresetResetSettings(
        std::string&,
        std::string_view,
        std::string_view,
        std::string&);
    PresetSelections GetSavedPresetSelections(std::string&, std::string&);
    bool SavePresetSelectionSnapshot(
        std::string&,
        const PresetSelections&,
        std::string_view,
        std::string&);
    bool ApplyPresetPreview(std::string&, std::string_view, std::string_view, std::string&);
    bool SaveSettings(std::string&);
    bool SaveProfileSetupSettings(std::string&, ProfileSetup::Domain, std::string&);
    bool RestoreProfileSetupSettings(std::string&, ProfileSetup::Domain, std::string&);
    bool SavePageSettings(std::string&, const std::vector<std::string>&);
    bool RestoreSettings(std::string&);
    bool RestorePageSettings(std::string&, const std::vector<std::string>&);
    bool ResetAllSettingsToDefault(std::string&);
    bool ResetSettingsToDefault(std::string&, const std::vector<std::string>&);
}  // namespace MPL::TuningUtil
