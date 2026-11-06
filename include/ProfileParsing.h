#pragma once

#include <TuningUtil.h>
#include <yyjson.h>

namespace MPL::TuningUtil::ProfileParsing
{
    std::string Lowercase(std::string);
    std::string Trim(std::string);
    struct ParsedFilteredWeatherSetting
    {
        FilteredWeatherSetting setting;
    };

    struct SliderSettingSpec
    {
        std::string path;
        double scale = 1.0;
    };

    struct LightingSliderLinkRules
    {
        std::vector<std::string> settings;
        std::map<std::string, LightingPatcher::LightingLinks, std::less<>> customLinks;
        std::vector<std::string> declaredSettings;
        std::vector<std::string> weatherDeclaredSettings;
    };

    struct ProfileLayout
    {
        std::string profile;
        std::vector<SliderStorage::Binding> bindings;
        std::vector<FilteredWeatherRule> weatherRules;
        std::vector<FilteredLightingTemplateRule> lightingRules;
        std::vector<FilteredBaseLightRule> baseLightRules;
        std::vector<FilteredObjectLightingRule> objectLightingRules;
        LightingSliderLinkRules lightingLinks;
        std::string text;
    };

    std::optional<std::string> JsonString(yyjson_val* a_object, const std::string_view a_key);
    std::optional<std::string> ReadProfileName(yyjson_val*, const std::filesystem::path&, std::string&);
    std::optional<int> JsonInteger(yyjson_val* a_object, const std::string_view a_key);
    std::vector<std::string> JsonStrings(yyjson_val* a_object, const std::string_view a_key);
    WeatherFilter JsonWeatherFilter(yyjson_val* a_control, const std::string_view a_key);
    WeatherPatcher::SettingLink* WeatherCustomLinkField(
        WeatherPatcher::WeatherLinks& a_links,
        const std::string_view a_field);
    WeatherPatcher::SettingLink* LightingCustomLinkField(
        LightingPatcher::LightingLinks& a_links,
        const std::string_view a_field);
    std::optional<WeatherPatcher::WeatherLinks> JsonCustomWeatherLinks(yyjson_val* a_control);
    std::optional<LightingPatcher::LightingLinks> JsonCustomLightingLinks(yyjson_val* a_control);
    std::optional<ParsedFilteredWeatherSetting> ParseFilteredWeatherSetting(const std::string_view a_setting);
    std::optional<FilteredLightingTemplateSetting> ParseFilteredLightingTemplateSetting(
        const std::string_view a_setting);
    std::optional<FilteredBaseLightSetting> ParseFilteredBaseLightSetting(
        const std::string_view a_setting);
    std::optional<FilteredObjectLightingSetting> ParseFilteredObjectLightingSetting(
        const std::string_view a_setting);
    std::optional<WeatherPatcher::AmbientHueScales> JsonHueScales(yyjson_val* a_control);
    std::vector<SliderSettingSpec> JsonSliderSettings(yyjson_val* a_control);
    bool IsLightingLinkableSliderSetting(const std::string_view a_setting);
    bool ValidSliderID(const std::string_view a_id);
    bool ParseTimes(
        yyjson_val* a_control,
        std::array<bool, RE::TESWeather::ColorTime::kTotal>& a_times,
        const std::string_view a_id,
        const std::filesystem::path& a_source);
    std::optional<FilteredWeatherRule> MakeFilteredWeatherRule(
        yyjson_val* a_control,
        std::string a_id,
        std::string a_controlID,
        std::vector<SliderSettingSpec> a_settings,
        const std::filesystem::path& a_source);
    std::vector<FilteredWeatherRule> ParseFilteredWeatherRules(
        yyjson_val* a_control,
        const std::filesystem::path& a_source);
    std::optional<FilteredLightingTemplateRule> ParseFilteredLightingTemplateRule(
        yyjson_val* a_control,
        const std::filesystem::path& a_source);
    std::optional<FilteredBaseLightRule> ParseFilteredBaseLightRule(
        yyjson_val* a_control,
        const std::filesystem::path& a_source);
    std::optional<FilteredObjectLightingRule> ParseFilteredObjectLightingRule(
        yyjson_val* a_control,
        const std::filesystem::path& a_source);
    std::vector<FilteredWeatherRule> ReadFilteredWeatherRules(yyjson_val* root, const std::filesystem::path& path);
    std::vector<FilteredLightingTemplateRule> ReadFilteredLightingTemplateRules(
        yyjson_val* root, const std::filesystem::path& path);
    std::vector<FilteredBaseLightRule> ReadFilteredBaseLightRules(
        yyjson_val* root, const std::filesystem::path& path);
    std::vector<FilteredObjectLightingRule> ReadFilteredObjectLightingRules(
        yyjson_val* root, const std::filesystem::path& path);
    LightingSliderLinkRules ReadLightingSliderLinkRules(yyjson_val* root);
    std::optional<Settings> ParseSettings(const std::string&, const std::filesystem::path&);
    std::optional<ProfileLayout> ParseLayout(std::string_view, const std::filesystem::path&, std::string&);
}
