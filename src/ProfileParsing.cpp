#include <ProfileParsing.h>
#include <Config/Common.h>
#include <JsonOverlay.h>
#include <SliderSettingCatalog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <memory>

namespace MPL::TuningUtil::ProfileParsing
{
    std::optional<ProfileLayout> ParseLayout(const std::string_view a_text, const std::filesystem::path& a_path,
        std::string& a_error)
    {
        a_error.clear();
        const std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
            yyjson_read(a_text.data(), a_text.size(), 0), yyjson_doc_free);
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        auto* pages = yyjson_is_obj(root) ? yyjson_obj_get(root, "pages") : nullptr;
        auto* version = yyjson_is_obj(root) ? yyjson_obj_get(root, "schemaVersion") : nullptr;
        if (!yyjson_is_arr(pages) || !yyjson_is_num(version) || yyjson_get_num(version) != 1.0)
        {
            a_error = std::format("The profile layout could not be read or has an invalid schema: {}", a_path.string());
            return std::nullopt;
        }
        std::size_t index, count;
        yyjson_val* page;
        yyjson_arr_foreach(pages, index, count, page)
        {
            if (!yyjson_is_obj(page) || !yyjson_is_arr(yyjson_obj_get(page, "modules")))
            {
                a_error = std::format("The profile layout contains an invalid page: {}", a_path.string());
                return std::nullopt;
            }
        }
        auto bindings = SliderStorage::ReadLayout(root, a_error);
        if (!a_error.empty()) return std::nullopt;
        auto name = ReadProfileName(root, a_path, a_error);
        if (!name) return std::nullopt;
        ProfileLayout result{ .profile = std::move(*name), .bindings = std::move(bindings) };
#ifndef TUNING_ATLM
        result.weatherRules = ReadFilteredWeatherRules(root, a_path);
#endif
#ifndef TUNING_HELIOS
        result.lightingRules = ReadFilteredLightingTemplateRules(root, a_path);
        result.lightingLinks = ReadLightingSliderLinkRules(root);
#endif
#ifndef TUNING_HEADLESS
        result.baseLightRules = ReadFilteredBaseLightRules(root, a_path);
        result.objectLightingRules = ReadFilteredObjectLightingRules(root, a_path);
        result.text = a_text;
#endif
        return result;
    }

    std::string Lowercase(std::string a_value)
    {
        std::ranges::transform(a_value, a_value.begin(), [](const unsigned char a_character)
            { return static_cast<char>(std::tolower(a_character)); });
        return a_value;
    }

    std::optional<std::string> ReadProfileName(yyjson_val* a_root, const std::filesystem::path& a_path,
        std::string& a_error)
    {
        a_error.clear();
        if (yyjson_is_obj(a_root))
        {
            if (!yyjson_obj_get(a_root, "profile")) return std::string{};
            auto name = Trim(JsonString(a_root, "profile").value_or(""));
            if (!name.empty()) return name;
        }
        a_error = std::format("The profile layout contains an invalid profile name: {}", a_path.string());
        return std::nullopt;
    }

    std::string Trim(std::string a_value)
    {
        const auto first = a_value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = a_value.find_last_not_of(" \t\r\n");
        return a_value.substr(first, last - first + 1);
    }

    std::optional<std::string> JsonString(yyjson_val* a_object, const std::string_view a_key)
    {
        auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
        return yyjson_is_str(value) ?
                   std::optional<std::string>{ std::string(yyjson_get_str(value), yyjson_get_len(value)) } :
                   std::nullopt;
    }

    std::optional<int> JsonInteger(yyjson_val* a_object, const std::string_view a_key)
    {
        auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
        if (!yyjson_is_int(value) && !yyjson_is_uint(value)) return std::nullopt;
        const auto number = yyjson_get_sint(value);
        return number >= (std::numeric_limits<int>::min)() && number <= (std::numeric_limits<int>::max)() ?
                   std::optional<int>{ static_cast<int>(number) } :
                   std::nullopt;
    }

    std::vector<std::string> JsonStrings(yyjson_val* a_object, const std::string_view a_key)
    {
        std::vector<std::string> result;
        auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
        const auto append = [&](yyjson_val* a_item)
        {
            if (!yyjson_is_str(a_item))
            {
                return;
            }
            auto text = Trim(std::string(yyjson_get_str(a_item), yyjson_get_len(a_item)));
            if (!text.empty() && !std::ranges::any_of(result, [&](const auto& a_existing)
                    { return Config::IEquals(a_existing, text); }))
            {
                result.push_back(std::move(text));
            }
        };
        if (yyjson_is_str(value))
        {
            append(value);
        }
        else if (yyjson_is_arr(value))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(value, index, maximum, item) append(item);
        }
        return result;
    }

    WeatherFilter JsonWeatherFilter(yyjson_val* a_control, const std::string_view a_key)
    {
        WeatherFilter result;
        auto* filter = yyjson_is_obj(a_control) ? yyjson_obj_getn(a_control, a_key.data(), a_key.size()) : nullptr;
        result.formIDs = JsonStrings(filter, "formIDs");
        result.contains = JsonStrings(filter, "contains");
        return result;
    }

    WeatherPatcher::SettingLink* WeatherCustomLinkField(
        WeatherPatcher::WeatherLinks& a_links,
        const std::string_view a_field)
    {
        if (Config::IEquals(a_field, "ambient")) return &a_links.ambient;
        if (Config::IEquals(a_field, "sunlight")) return &a_links.sunlight;
        if (Config::IEquals(a_field, "effectLighting")) return &a_links.effectLighting;
        if (Config::IEquals(a_field, "fogFar")) return &a_links.fogFar;
        if (Config::IEquals(a_field, "fogNear")) return &a_links.fogNear;
        if (Config::IEquals(a_field, "water")) return &a_links.water;
        if (Config::IEquals(a_field, "skyStatics")) return &a_links.skyStatics;
        if (Config::IEquals(a_field, "skyUpper")) return &a_links.skyUpper;
        if (Config::IEquals(a_field, "skyLower")) return &a_links.skyLower;
        if (Config::IEquals(a_field, "horizon")) return &a_links.horizon;
        if (Config::IEquals(a_field, "sun")) return &a_links.sun;
        if (Config::IEquals(a_field, "sunGlare")) return &a_links.sunGlare;
        if (Config::IEquals(a_field, "moonGlare")) return &a_links.moonGlare;
        if (Config::IEquals(a_field, "stars")) return &a_links.stars;
        if (Config::IEquals(a_field, "cloudLayers")) return &a_links.cloudLayers;
        if (Config::IEquals(a_field, "volumetricLighting")) return &a_links.volumetricLighting;
        return nullptr;
    }

    WeatherPatcher::SettingLink* LightingCustomLinkField(
        LightingPatcher::LightingLinks& a_links,
        const std::string_view a_field)
    {
        if (Config::IEquals(a_field, "ambient")) return &a_links.ambient;
        if (Config::IEquals(a_field, "directional")) return &a_links.directional;
        if (Config::IEquals(a_field, "ambientColors")) return &a_links.ambientColors;
        if (Config::IEquals(a_field, "fogFar")) return &a_links.fogFar;
        if (Config::IEquals(a_field, "fogNear")) return &a_links.fogNear;
        return nullptr;
    }

    template <class Links, class Field>
    std::optional<Links> JsonCustomLinks(
        yyjson_val* a_control,
        const std::span<const std::string_view> a_fields,
        Field&& a_field)
    {
        auto* object = yyjson_is_obj(a_control) ? yyjson_obj_get(a_control, "customLinks") : nullptr;
        if (!yyjson_is_obj(object)) return std::nullopt;

        Links result;
        for (const auto field : a_fields)
            if (auto* value = a_field(result, field)) value->reset();

        std::size_t index = 0;
        std::size_t maximum = 0;
        yyjson_val* key = nullptr;
        yyjson_val* value = nullptr;
        yyjson_obj_foreach(object, index, maximum, key, value)
        {
            if (!yyjson_is_str(key) || !yyjson_is_arr(value)) continue;
            auto* source = yyjson_arr_get(value, 0);
            auto* scale = yyjson_arr_get(value, 1);
            if (!yyjson_is_str(source)) continue;
            const auto targetName = std::string_view(yyjson_get_str(key), yyjson_get_len(key));
            const auto sourceName = Trim(std::string(yyjson_get_str(source), yyjson_get_len(source)));
            const auto scaleValue = yyjson_is_num(scale) ? yyjson_get_real(scale) : 1.0;
            if (sourceName.empty() || !std::isfinite(scaleValue)) continue;
            if (auto* target = a_field(result, targetName))
                *target = std::tuple{ sourceName, scaleValue };
        }
        return result;
    }

    std::optional<WeatherPatcher::WeatherLinks> JsonCustomWeatherLinks(yyjson_val* a_control)
    {
        static constexpr std::array fields{
            std::string_view{ "ambient" }, std::string_view{ "sunlight" },
            std::string_view{ "effectLighting" }, std::string_view{ "fogFar" },
            std::string_view{ "fogNear" }, std::string_view{ "water" },
            std::string_view{ "skyStatics" }, std::string_view{ "skyUpper" },
            std::string_view{ "skyLower" }, std::string_view{ "horizon" },
            std::string_view{ "sun" }, std::string_view{ "sunGlare" },
            std::string_view{ "moonGlare" }, std::string_view{ "stars" },
            std::string_view{ "cloudLayers" }, std::string_view{ "volumetricLighting" },
        };
        return JsonCustomLinks<WeatherPatcher::WeatherLinks>(a_control, fields, WeatherCustomLinkField);
    }

    std::optional<LightingPatcher::LightingLinks> JsonCustomLightingLinks(yyjson_val* a_control)
    {
        static constexpr std::array fields{
            std::string_view{ "ambient" }, std::string_view{ "directional" },
            std::string_view{ "ambientColors" }, std::string_view{ "fogFar" },
            std::string_view{ "fogNear" },
        };
        return JsonCustomLinks<LightingPatcher::LightingLinks>(a_control, fields, LightingCustomLinkField);
    }


    std::optional<ParsedFilteredWeatherSetting> ParseFilteredWeatherSetting(const std::string_view a_setting)
    {
        const auto* entry = SliderSettingCatalog::Find(a_setting);
        if (!entry ||
            entry->domain != SliderSettingCatalog::Domain::weather ||
            !SliderSettingCatalog::IsFilteredOperation(entry->filterOperation))
            return std::nullopt;
        ParsedFilteredWeatherSetting result;
        result.setting.target = Lowercase(entry->target);
        switch (entry->filterOperation)
        {
        case SliderSettingCatalog::FilterOperation::brightness:
            result.setting.operation = FilteredWeatherOperation::brightness;
            break;
        case SliderSettingCatalog::FilterOperation::saturation:
            result.setting.operation = FilteredWeatherOperation::saturation;
            break;
        case SliderSettingCatalog::FilterOperation::hueShift:
            result.setting.operation = FilteredWeatherOperation::hueShift;
            if (!entry->hue.empty()) result.setting.hue = Lowercase(entry->hue);
            break;
        default:
            return std::nullopt;
        }
        return result;
    }

    std::optional<FilteredLightingTemplateSetting> ParseFilteredLightingTemplateSetting(
        const std::string_view a_setting)
    {
        const auto* entry = SliderSettingCatalog::Find(a_setting);
        if (!entry || entry->domain != SliderSettingCatalog::Domain::lighting)
        {
            return std::nullopt;
        }
        if (a_setting.starts_with("lightBrightnessMultiplier.") && !entry->target.empty())
        {
            return FilteredLightingTemplateSetting{
                .operation = FilteredLightingTemplateOperation::brightness,
                .target = Lowercase(entry->target),
            };
        }
        if (a_setting.starts_with("lightSaturationMultiplier.") && !entry->target.empty())
        {
            return FilteredLightingTemplateSetting{
                .operation = FilteredLightingTemplateOperation::saturation,
                .target = Lowercase(entry->target),
            };
        }
        if (a_setting == "lightFogMaxMultiplier")
        {
            return FilteredLightingTemplateSetting{
                .operation = FilteredLightingTemplateOperation::fogStrength,
            };
        }
        if (a_setting == "lightFogPowerMultiplier")
        {
            return FilteredLightingTemplateSetting{
                .operation = FilteredLightingTemplateOperation::fogPower,
            };
        }
        return std::nullopt;
    }

    std::optional<FilteredBaseLightSetting> ParseFilteredBaseLightSetting(
        const std::string_view a_setting)
    {
        const auto* entry = SliderSettingCatalog::Find(a_setting);
        if (!entry || entry->domain != SliderSettingCatalog::Domain::lighting ||
            !entry->path.starts_with("pointLights."))
            return std::nullopt;

        FilteredBaseLightSetting result;
        if (entry->path == "pointLights.fadeMultiplier")
            result.operation = FilteredBaseLightOperation::brightness;
        else if (entry->path == "pointLights.radiusMultiplier")
            result.operation = FilteredBaseLightOperation::radius;
        else if (entry->path == "pointLights.saturationMultiplier")
            result.operation = FilteredBaseLightOperation::saturation;
        else if (entry->path.starts_with("pointLights.hueShift.") && !entry->hue.empty())
        {
            result.operation = FilteredBaseLightOperation::hueShift;
            result.hue = Lowercase(entry->hue);
        }
        else
            return std::nullopt;
        return result;
    }

    std::optional<FilteredObjectLightingSetting> ParseFilteredObjectLightingSetting(
        const std::string_view a_setting)
    {
        const auto* entry = SliderSettingCatalog::Find(a_setting);
        if (!entry || entry->domain != SliderSettingCatalog::Domain::lighting)
            return std::nullopt;
        if (entry->path == "objectEffectLighting.emissiveMultiplier")
        {
            return FilteredObjectLightingSetting{
                .operation = FilteredObjectLightingOperation::emissiveMultiplier,
            };
        }
        if (entry->path == "objectEffectLighting.baseColorScale")
        {
            return FilteredObjectLightingSetting{
                .operation = FilteredObjectLightingOperation::baseColorScale,
            };
        }
        return std::nullopt;
    }

    std::optional<WeatherPatcher::AmbientHueScales> JsonHueScales(yyjson_val* a_control)
    {
        auto* object = yyjson_is_obj(a_control) ? yyjson_obj_get(a_control, "hueScales") : nullptr;
        if (!yyjson_is_obj(object)) return std::nullopt;
        const auto value = [&](const char* a_key)
        {
            auto* member = yyjson_obj_get(object, a_key);
            const auto number = yyjson_is_num(member) ? yyjson_get_real(member) : 1.0;
            return std::isfinite(number) ? number : 1.0;
        };
        return WeatherPatcher::AmbientHueScales{
            value("red"), value("orange"), value("yellow"), value("green"),
            value("teal"), value("blue"), value("magenta"),
        };
    }


    std::vector<SliderSettingSpec> JsonSliderSettings(yyjson_val* a_control)
    {
        std::vector<SliderSettingSpec> result;
        const auto append = [&](yyjson_val* a_value)
        {
            if (yyjson_is_str(a_value))
            {
                auto path = Trim(std::string(yyjson_get_str(a_value), yyjson_get_len(a_value)));
                if (!path.empty() && !std::ranges::any_of(result, [&](const auto& a_existing)
                        { return Config::IEquals(a_existing.path, path); }))
                    result.push_back({ std::move(path), 1.0 });
                return;
            }
            if (!yyjson_is_obj(a_value)) return;

            auto path = JsonString(a_value, "setting");
            if (!path || (path = Trim(std::move(*path)), path->empty())) return;
            double scale = 1.0;
            if (auto* value = yyjson_obj_get(a_value, "scale"); yyjson_is_num(value))
            {
                const auto candidate = yyjson_get_real(value);
                if (std::isfinite(candidate)) scale = candidate;
            }
            if (!std::ranges::any_of(result, [&](const auto& a_existing)
                    { return Config::IEquals(a_existing.path, *path); }))
                result.push_back({ std::move(*path), scale });
        };

        auto* settings = yyjson_obj_get(a_control, "settings");
        if (yyjson_is_arr(settings))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(settings, index, maximum, item) append(item);
        }
        else if (settings)
        {
            append(settings);
        }
        if (result.empty())
        {
            if (auto* setting = yyjson_obj_get(a_control, "setting")) append(setting);
        }
        return result;
    }

    bool IsLightingLinkableSliderSetting(const std::string_view a_setting)
    {
        return a_setting.starts_with("lightBrightnessMultiplier.") ||
               a_setting.starts_with("lightSaturationMultiplier.") ||
               a_setting.starts_with("lightHueShift.");
    }

    bool ValidSliderID(const std::string_view a_id)
    {
        return !a_id.empty() && std::ranges::all_of(a_id, [](const unsigned char a_character)
        {
            return a_character > ' ';
        });
    }

    bool ParseTimes(
        yyjson_val* a_control,
        std::array<bool, RE::TESWeather::ColorTime::kTotal>& a_times,
        const std::string_view a_id,
        const std::filesystem::path& a_source)
    {
        const auto configuredTimes = JsonStrings(a_control, "times");
        if (configuredTimes.empty())
        {
            a_times.fill(true);
            return true;
        }
        for (const auto& configured : configuredTimes)
        {
            const auto time = Lowercase(configured);
            if (time == "all") a_times.fill(true);
            else if (time == "sunrise") a_times[RE::TESWeather::ColorTime::kSunrise] = true;
            else if (time == "day") a_times[RE::TESWeather::ColorTime::kDay] = true;
            else if (time == "sunset") a_times[RE::TESWeather::ColorTime::kSunset] = true;
            else if (time == "night") a_times[RE::TESWeather::ColorTime::kNight] = true;
            else if (time == "duskanddawn")
            {
                a_times[RE::TESWeather::ColorTime::kSunrise] = true;
                a_times[RE::TESWeather::ColorTime::kSunset] = true;
            }
            else
            {
                logger::warn("[TuningUtil] filtered slider={} | source={} | time={} unsupported", a_id, a_source.string(), configured);
                return false;
            }
        }
        return true;
    }

    std::optional<FilteredWeatherRule> MakeFilteredWeatherRule(
        yyjson_val* a_control,
        std::string a_id,
        std::string a_controlID,
        std::vector<SliderSettingSpec> a_settings,
        const std::filesystem::path& a_source)
    {
        if (!ValidSliderID(a_id))
        {
            logger::warn("[TuningUtil] filtered slider ignored | source={} | id missing", a_source.string());
            return std::nullopt;
        }

        FilteredWeatherRule rule;
        rule.id = std::move(a_id);
        rule.controlID = std::move(a_controlID);
        rule.customLinks = JsonCustomWeatherLinks(a_control);
        if (auto* value = yyjson_obj_get(a_control, "ignoreProfileFilters"); yyjson_is_bool(value))
            rule.ignoreProfileFilters = yyjson_get_bool(value);
        rule.hueScales = JsonHueScales(a_control);
        for (const auto& specification : a_settings)
        {
            auto parsed = ParseFilteredWeatherSetting(specification.path);
            if (!parsed)
            {
                logger::warn("[TuningUtil] filtered slider={} | source={} | setting={} unsupported", rule.id, a_source.string(), specification.path);
                return std::nullopt;
            }
            if (!rule.settings.empty() && parsed->setting.operation != rule.settings.front().operation)
            {
                logger::warn("[TuningUtil] filtered slider={} | source={} | grouped operations conflict", rule.id, a_source.string());
                return std::nullopt;
            }
            parsed->setting.scale = specification.scale;
            rule.settings.push_back(std::move(parsed->setting));
        }
        if (rule.settings.empty() || !ParseTimes(a_control, rule.times, rule.id, a_source))
        {
            return std::nullopt;
        }
        if (rule.hueScales && rule.settings.front().operation != FilteredWeatherOperation::saturation)
        {
            logger::warn("[TuningUtil] filtered slider={} | source={} | saturation settings required",
                rule.id, a_source.string());
            return std::nullopt;
        }
        rule.defaultValue = rule.settings.front().operation == FilteredWeatherOperation::hueShift ? 0.0 : 1.0;
        rule.include = JsonWeatherFilter(yyjson_obj_get(a_control, "weatherFilter"), "include");
        rule.exclude = JsonWeatherFilter(yyjson_obj_get(a_control, "weatherFilter"), "exclude");
        return rule;
    }

    std::vector<FilteredWeatherRule> ParseFilteredWeatherRules(
        yyjson_val* a_control,
        const std::filesystem::path& a_source)
    {
        std::vector<FilteredWeatherRule> result;
        if (yyjson_is_obj(yyjson_obj_get(a_control, "lightingTemplateFilter")) ||
            yyjson_is_obj(yyjson_obj_get(a_control, "baseLightFilter")) ||
            yyjson_is_obj(yyjson_obj_get(a_control, "baseObjectFilter")))
            return result;
        const auto kind = Lowercase(Trim(JsonString(a_control, "type").value_or("")));
        const auto times = JsonStrings(a_control, "times");
        const auto filtered = !times.empty() || yyjson_is_obj(yyjson_obj_get(a_control, "weatherFilter")) ||
                              yyjson_is_obj(yyjson_obj_get(a_control, "hueScales")) ||
                              yyjson_is_bool(yyjson_obj_get(a_control, "ignoreProfileFilters"));
        if (!filtered || (kind != "slider" && kind != "settings")) return result;

        const auto controlID = Trim(JsonString(a_control, "id").value_or(""));
        if (kind == "slider")
        {
            auto settings = JsonSliderSettings(a_control);
            if (auto rule = MakeFilteredWeatherRule(a_control, controlID, controlID, std::move(settings), a_source))
            {
                result.push_back(std::move(*rule));
            }
            return result;
        }

        if (!ValidSliderID(controlID))
        {
            logger::warn("[TuningUtil] filtered editor ignored | source={} | id missing", a_source.string());
            return result;
        }
        const auto category = Lowercase(Trim(JsonString(a_control, "setting").value_or("")));
        static constexpr std::array targets{
            std::string_view{ "ambient" }, std::string_view{ "sunlight" }, std::string_view{ "effectLighting" },
            std::string_view{ "fogFar" }, std::string_view{ "fogNear" }, std::string_view{ "water" },
            std::string_view{ "skyStatics" }, std::string_view{ "skyUpper" }, std::string_view{ "skyLower" },
            std::string_view{ "horizon" }, std::string_view{ "sun" }, std::string_view{ "sunGlare" },
            std::string_view{ "moonGlare" }, std::string_view{ "stars" }, std::string_view{ "cloudLayers" },
            std::string_view{ "volumetricLighting" },
        };
        const auto add = [&](const std::string& a_suffix, std::vector<std::string> a_settings)
        {
            std::vector<SliderSettingSpec> specifications;
            specifications.reserve(a_settings.size());
            for (auto& setting : a_settings)
                specifications.push_back({ std::move(setting), 1.0 });
            if (auto rule = MakeFilteredWeatherRule(
                    a_control,
                    controlID + "_" + a_suffix,
                    controlID,
                    std::move(specifications),
                    a_source))
            {
                result.push_back(std::move(*rule));
            }
        };
        if (category == "brightness")
        {
            for (const auto target : targets)
                add(Lowercase(std::string(target)), { "brightnessMultiplier." + std::string(target) });
        }
        else if (category == "saturation")
        {
            for (const auto target : targets)
                add(Lowercase(std::string(target)), { "saturationMultiplier." + std::string(target) });
        }
        else if (category == "hueshift")
        {
            static constexpr std::array hues{
                std::string_view{ "red" }, std::string_view{ "orange" }, std::string_view{ "yellow" },
                std::string_view{ "green" }, std::string_view{ "teal" }, std::string_view{ "blue" },
                std::string_view{ "magenta" },
            };
            for (const auto target : targets)
                for (const auto hue : hues)
                    add(Lowercase(std::string(target)) + "_" + std::string(hue),
                        { "hueShift." + std::string(target) + "." + std::string(hue) });
        }
        return result;
    }

    std::optional<FilteredLightingTemplateRule> ParseFilteredLightingTemplateRule(
        yyjson_val* a_control,
        const std::filesystem::path& a_source)
    {
        if (!yyjson_is_obj(yyjson_obj_get(a_control, "lightingTemplateFilter")) ||
            !Config::IEquals(Trim(JsonString(a_control, "type").value_or("")), "slider"))
        {
            return std::nullopt;
        }

        const auto id = Trim(JsonString(a_control, "id").value_or(""));
        if (!ValidSliderID(id))
        {
            logger::warn("[TuningUtil] filtered Lighting Template slider ignored | source={} | id missing", a_source.string());
            return std::nullopt;
        }

        FilteredLightingTemplateRule rule{
            .id = id,
            .controlID = id,
            .customLinks = JsonCustomLightingLinks(a_control),
            .hueScales = JsonHueScales(a_control),
        };
        if (auto* value = yyjson_obj_get(a_control, "ignoreProfileFilters"); yyjson_is_bool(value))
            rule.ignoreProfileFilters = yyjson_get_bool(value);
        for (const auto& specification : JsonSliderSettings(a_control))
        {
            auto setting = ParseFilteredLightingTemplateSetting(specification.path);
            if (!setting)
            {
                logger::warn(
                    "[TuningUtil] filtered Lighting Template slider={} | source={} | setting={} unsupported",
                    rule.id,
                    a_source.string(),
                    specification.path);
                return std::nullopt;
            }
            setting->scale = specification.scale;
            rule.settings.push_back(std::move(*setting));
        }
        if (rule.settings.empty() || !std::ranges::all_of(rule.settings, [&](const auto& a_setting)
                { return a_setting.operation == rule.settings.front().operation; }) || (rule.hueScales &&
            !std::ranges::all_of(rule.settings, [](const auto& a_setting)
                { return a_setting.operation == FilteredLightingTemplateOperation::saturation; })))
        {
            logger::warn(
                "[TuningUtil] filtered Lighting Template slider={} | source={} | settings unsupported",
                rule.id,
                a_source.string());
            return std::nullopt;
        }
        rule.include = JsonWeatherFilter(yyjson_obj_get(a_control, "lightingTemplateFilter"), "include");
        rule.exclude = JsonWeatherFilter(yyjson_obj_get(a_control, "lightingTemplateFilter"), "exclude");
        rule.locationTypeInclusions =
            JsonStrings(yyjson_obj_get(yyjson_obj_get(a_control, "lightingTemplateFilter"), "include"), "locationTypes");
        rule.locationTypeExclusions =
            JsonStrings(yyjson_obj_get(yyjson_obj_get(a_control, "lightingTemplateFilter"), "exclude"), "locationTypes");
        rule.inclusionMultiLocationExceptions = JsonStrings(
            yyjson_obj_get(yyjson_obj_get(a_control, "lightingTemplateFilter"), "include"),
            "multiLocationExceptions");
        rule.exclusionMultiLocationExceptions = JsonStrings(
            yyjson_obj_get(yyjson_obj_get(a_control, "lightingTemplateFilter"), "exclude"),
            "multiLocationExceptions");
        return rule;
    }

    std::optional<FilteredBaseLightRule> ParseFilteredBaseLightRule(
        yyjson_val* a_control,
        const std::filesystem::path& a_source)
    {
        if (!yyjson_is_obj(yyjson_obj_get(a_control, "baseLightFilter")) ||
            !Config::IEquals(Trim(JsonString(a_control, "type").value_or("")), "slider"))
            return std::nullopt;

        const auto id = Trim(JsonString(a_control, "id").value_or(""));
        if (!ValidSliderID(id))
        {
            logger::warn("[TuningUtil] filtered Base Light slider ignored | source={} | id missing", a_source.string());
            return std::nullopt;
        }

        FilteredBaseLightRule rule{
            .id = id,
            .controlID = id,
            .priority = std::clamp(JsonInteger(a_control, "priority").value_or(0), 0, 99),
            .hueScales = JsonHueScales(a_control),
        };
        std::optional<SliderSettingCatalog::FilterOperation> operation;
        for (const auto& specification : JsonSliderSettings(a_control))
        {
            const auto* entry = SliderSettingCatalog::Find(specification.path);
            auto setting = ParseFilteredBaseLightSetting(specification.path);
            if (!entry || !setting || !SliderSettingCatalog::IsFilteredOperation(entry->filterOperation))
            {
                logger::warn(
                    "[TuningUtil] filtered Base Light slider={} | source={} | setting={} unsupported",
                    rule.id,
                    a_source.string(),
                    specification.path);
                return std::nullopt;
            }
            if (operation && *operation != entry->filterOperation)
            {
                logger::warn(
                    "[TuningUtil] filtered Base Light slider={} | source={} | grouped operations conflict",
                    rule.id,
                    a_source.string());
                return std::nullopt;
            }
            operation = entry->filterOperation;
            setting->scale = specification.scale;
            rule.settings.push_back(std::move(*setting));
        }
        if (rule.settings.empty() || (rule.hueScales &&
            *operation != SliderSettingCatalog::FilterOperation::saturation)) return std::nullopt;
        rule.defaultValue = rule.settings.front().operation == FilteredBaseLightOperation::hueShift ? 0.0 : 1.0;
        rule.include = JsonWeatherFilter(yyjson_obj_get(a_control, "baseLightFilter"), "include");
        rule.exclude = JsonWeatherFilter(yyjson_obj_get(a_control, "baseLightFilter"), "exclude");
        auto* hueFilter = yyjson_obj_get(a_control, "hueFilter");
        rule.hueFilter.include = JsonStrings(hueFilter, "include");
        rule.useXemiFilter = yyjson_is_obj(yyjson_obj_get(a_control, "xemiFilter"));
        if (rule.useXemiFilter && *operation != SliderSettingCatalog::FilterOperation::brightness &&
            *operation != SliderSettingCatalog::FilterOperation::radius)
        {
            logger::warn("[TuningUtil] filtered Base Light slider={} | XEMI requires Brightness or Radius", rule.id);
            return std::nullopt;
        }
        rule.xemiInclude = JsonWeatherFilter(yyjson_obj_get(a_control, "xemiFilter"), "include");
        rule.xemiExclude = JsonWeatherFilter(yyjson_obj_get(a_control, "xemiFilter"), "exclude");
        if (!HueFilter::Valid(rule.hueFilter))
        {
            logger::warn("[TuningUtil] filtered Base Light slider={} | source={} | invalid hue band",
                rule.id, a_source.string());
            return std::nullopt;
        }
        return rule;
    }

    std::optional<FilteredObjectLightingRule> ParseFilteredObjectLightingRule(
        yyjson_val* a_control,
        const std::filesystem::path& a_source)
    {
        if (!yyjson_is_obj(yyjson_obj_get(a_control, "baseObjectFilter")) ||
            !Config::IEquals(Trim(JsonString(a_control, "type").value_or("")), "slider"))
            return std::nullopt;

        const auto id = Trim(JsonString(a_control, "id").value_or(""));
        if (!ValidSliderID(id))
        {
            logger::warn("[TuningUtil] filtered Object Effect Lighting slider ignored | source={} | id missing", a_source.string());
            return std::nullopt;
        }

        FilteredObjectLightingRule rule{
            .id = id,
            .controlID = id,
            .priority = std::clamp(JsonInteger(a_control, "priority").value_or(0), 0, 99),
        };
        for (const auto& specification : JsonSliderSettings(a_control))
        {
            auto setting = ParseFilteredObjectLightingSetting(specification.path);
            if (!setting)
            {
                logger::warn(
                    "[TuningUtil] filtered Object Effect Lighting slider={} | source={} | setting={} unsupported",
                    rule.id,
                    a_source.string(),
                    specification.path);
                return std::nullopt;
            }
            setting->scale = specification.scale;
            rule.settings.push_back(std::move(*setting));
        }
        if (rule.settings.empty()) return std::nullopt;
        rule.include = JsonWeatherFilter(yyjson_obj_get(a_control, "baseObjectFilter"), "include");
        rule.exclude = JsonWeatherFilter(yyjson_obj_get(a_control, "baseObjectFilter"), "exclude");
        rule.xemiInclude = JsonWeatherFilter(yyjson_obj_get(a_control, "xemiFilter"), "include");
        rule.xemiExclude = JsonWeatherFilter(yyjson_obj_get(a_control, "xemiFilter"), "exclude");
        auto* cellFilter = yyjson_obj_get(a_control, "cellFilter");
        if (yyjson_is_obj(cellFilter))
        {
            if (auto* interior = yyjson_obj_get(cellFilter, "interior"); yyjson_is_bool(interior))
                rule.interiorOnly = yyjson_get_bool(interior);
            if (auto* notInterior = yyjson_obj_get(cellFilter, "notInterior"); yyjson_is_bool(notInterior))
                rule.notInterior = yyjson_get_bool(notInterior);
            rule.cellInclude = JsonWeatherFilter(cellFilter, "include");
            rule.cellExclude = JsonWeatherFilter(cellFilter, "exclude");
        }
        return rule;
    }

    std::vector<FilteredWeatherRule> ReadFilteredWeatherRules(yyjson_val* root, const std::filesystem::path& path)
    {
        std::vector<FilteredWeatherRule> rules;
        const auto readModules = [&](yyjson_val* a_modules)
        {
            if (!yyjson_is_arr(a_modules))
            {
                return;
            }
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* control = nullptr;
            yyjson_arr_foreach(a_modules, index, maximum, control)
            {
                for (auto& rule : ParseFilteredWeatherRules(control, path))
                {
                    const auto duplicate = std::ranges::find_if(rules, [&](const FilteredWeatherRule& a_existing)
                        { return Config::IEquals(a_existing.id, rule.id); });
                    if (duplicate == rules.end())
                    {
                        rules.push_back(std::move(rule));
                    }
                    else if (*duplicate != rule)
                    {
                        logger::warn("[TuningUtil] filtered slider={} ignored | duplicate conflict | source={}", rule.id, path.string());
                    }
                }
            }
        };
        if (auto* pages = yyjson_obj_get(root, "pages"); yyjson_is_arr(pages))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* page = nullptr;
            yyjson_arr_foreach(pages, index, maximum, page) readModules(yyjson_obj_get(page, "modules"));
        }
        return rules;
    }

    std::vector<FilteredLightingTemplateRule> ReadFilteredLightingTemplateRules(
        yyjson_val* root, const std::filesystem::path& path)
    {
        std::vector<FilteredLightingTemplateRule> rules;
        const auto readModules = [&](yyjson_val* a_modules)
        {
            if (!yyjson_is_arr(a_modules)) return;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* control = nullptr;
            yyjson_arr_foreach(a_modules, index, maximum, control)
            {
                auto rule = ParseFilteredLightingTemplateRule(control, path);
                if (!rule) continue;
                const auto duplicate = std::ranges::find_if(rules, [&](const FilteredLightingTemplateRule& a_existing)
                    { return Config::IEquals(a_existing.id, rule->id); });
                if (duplicate == rules.end())
                {
                    rules.push_back(std::move(*rule));
                }
                else if (*duplicate != *rule)
                {
                    logger::warn(
                        "[TuningUtil] filtered Lighting Template slider={} ignored | duplicate conflict | source={}",
                        rule->id,
                        path.string());
                }
            }
        };
        if (auto* pages = yyjson_obj_get(root, "pages"); yyjson_is_arr(pages))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* page = nullptr;
            yyjson_arr_foreach(pages, index, maximum, page) readModules(yyjson_obj_get(page, "modules"));
        }
        return rules;
    }

    std::vector<FilteredBaseLightRule> ReadFilteredBaseLightRules(
        yyjson_val* root, const std::filesystem::path& path)
    {
        std::vector<FilteredBaseLightRule> rules;
        const auto readModules = [&](yyjson_val* a_modules)
        {
            if (!yyjson_is_arr(a_modules)) return;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* control = nullptr;
            yyjson_arr_foreach(a_modules, index, maximum, control)
            {
                auto rule = ParseFilteredBaseLightRule(control, path);
                if (!rule) continue;
                const auto duplicate = std::ranges::find_if(rules, [&](const FilteredBaseLightRule& a_existing)
                    { return Config::IEquals(a_existing.id, rule->id); });
                if (duplicate == rules.end())
                    rules.push_back(std::move(*rule));
                else if (*duplicate != *rule)
                    logger::warn(
                        "[TuningUtil] filtered Base Light slider={} ignored | duplicate conflict | source={}",
                        rule->id,
                        path.string());
            }
        };
        if (auto* pages = yyjson_obj_get(root, "pages"); yyjson_is_arr(pages))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* page = nullptr;
            yyjson_arr_foreach(pages, index, maximum, page) readModules(yyjson_obj_get(page, "modules"));
        }
        return rules;
    }

    std::vector<FilteredObjectLightingRule> ReadFilteredObjectLightingRules(
        yyjson_val* root, const std::filesystem::path& path)
    {
        std::vector<FilteredObjectLightingRule> rules;
        const auto readModules = [&](yyjson_val* a_modules)
        {
            if (!yyjson_is_arr(a_modules)) return;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* control = nullptr;
            yyjson_arr_foreach(a_modules, index, maximum, control)
            {
                auto rule = ParseFilteredObjectLightingRule(control, path);
                if (!rule) continue;
                const auto duplicate = std::ranges::find_if(
                    rules,
                    [&](const FilteredObjectLightingRule& a_existing)
                    { return Config::IEquals(a_existing.id, rule->id); });
                if (duplicate == rules.end())
                    rules.push_back(std::move(*rule));
                else if (*duplicate != *rule)
                    logger::warn(
                        "[TuningUtil] filtered Object Effect Lighting slider={} ignored | duplicate conflict | source={}",
                        rule->id,
                        path.string());
            }
        };
        if (auto* pages = yyjson_obj_get(root, "pages"); yyjson_is_arr(pages))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* page = nullptr;
            yyjson_arr_foreach(pages, index, maximum, page) readModules(yyjson_obj_get(page, "modules"));
        }
        return rules;
    }


    LightingSliderLinkRules ReadLightingSliderLinkRules(yyjson_val* root)
    {
        LightingSliderLinkRules result;
        auto weatherProfile = false;
        const auto add = [](std::vector<std::string>& a_values, const std::string& a_value)
        {
            if (!std::ranges::any_of(a_values, [&](const auto& a_existing)
                    { return Config::IEquals(a_existing, a_value); }))
                a_values.push_back(a_value);
        };
        const auto readModules = [&](yyjson_val* a_modules)
        {
            if (!yyjson_is_arr(a_modules)) return;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* control = nullptr;
            yyjson_arr_foreach(a_modules, index, maximum, control)
            {
                const auto type = Trim(JsonString(control, "type").value_or(""));
                const auto setting = Trim(JsonString(control, "setting").value_or(""));
                if (Config::IEquals(type, "timeOfDay") ||
                    Config::IEquals(type, "weatherSelector") ||
                    Config::IEquals(type, "weatherQuickSelect") ||
                    Config::IEquals(type, "ambientWithinGauge") ||
                    Config::IEquals(type, "ambientBetweenGauge") ||
                    Config::IEquals(type, "sunlightWithinGauge") ||
                    Config::IEquals(type, "sunlightBetweenGauge") ||
                    (Config::IEquals(type, "links") && Config::IEquals(setting, "weather")) ||
                    (Config::IEquals(type, "csTonemapping") && Config::IEquals(setting, "exteriorImageSpace")) ||
                    (Config::IEquals(type, "slider") && yyjson_is_obj(yyjson_obj_get(control, "weatherFilter"))))
                {
                    weatherProfile = true;
                }
                if (Config::IEquals(type, "links") && Config::IEquals(setting, "lighting"))
                {
                    add(result.declaredSettings, "links.lighting");
                    continue;
                }
                if (Config::IEquals(type, "settings"))
                {
                    if (Config::IEquals(setting, "ambientCompression") ||
                        Config::IEquals(setting, "sunlightCompression"))
                    {
                        weatherProfile = true;
                    }
                    else if (Config::IEquals(setting, "lightBrightness"))
                    {
                        add(result.declaredSettings, "lightBrightnessMultiplier");
                        add(result.declaredSettings, "lightFogPowerMultiplier");
                        add(result.declaredSettings, "lightFogMaxMultiplier");
                    }
                    else if (Config::IEquals(setting, "lightHueShift"))
                        add(result.declaredSettings, "lightHueShift");
                    continue;
                }
                if (!Config::IEquals(type, "slider") ||
                    yyjson_is_obj(yyjson_obj_get(control, "weatherFilter")) ||
                    yyjson_is_obj(yyjson_obj_get(control, "lightingTemplateFilter")) ||
                    yyjson_is_obj(yyjson_obj_get(control, "baseLightFilter")) ||
                    yyjson_is_obj(yyjson_obj_get(control, "baseObjectFilter")))
                    continue;

                const auto customLinks = JsonCustomLightingLinks(control);
                for (const auto& specification : JsonSliderSettings(control))
                {
                    if (!IsLightingLinkableSliderSetting(specification.path) ||
                        !SliderSettingCatalog::Find(specification.path))
                        continue;
                    add(result.settings, specification.path);
                    add(result.declaredSettings, specification.path);
                    if (customLinks)
                    {
                        result.customLinks.insert_or_assign(Lowercase(specification.path), *customLinks);
                    }
                }
            }
        };
        if (auto* pages = yyjson_obj_get(root, "pages"); yyjson_is_arr(pages))
        {
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* page = nullptr;
            yyjson_arr_foreach(pages, index, maximum, page) readModules(yyjson_obj_get(page, "modules"));
        }
        if (weatherProfile)
        {
            add(result.weatherDeclaredSettings, "links.weather");
        }
        else
        {
            add(result.declaredSettings, "links.lighting");
            add(result.declaredSettings, "lightHueRanges");
        }
        return result;
    }


    void ConstrainBetweenWeatherCompression(WeatherPatcher::CompressionSettings& a_settings)
    {
        const std::array values{
            &a_settings.ambient,
            &a_settings.sunlight,
            &a_settings.effectLighting,
            &a_settings.fogFar,
            &a_settings.fogNear,
            &a_settings.water,
            &a_settings.skyStatics,
            &a_settings.skyUpper,
            &a_settings.skyLower,
            &a_settings.horizon,
            &a_settings.sun,
            &a_settings.sunGlare,
            &a_settings.moonGlare,
            &a_settings.stars,
        };
        for (auto* value : values) *value = std::clamp(*value, -200.0, 100.0);
    }

    std::optional<Settings> ParseSettings(const std::string& a_json, const std::filesystem::path& a_source)
    {
        static constexpr std::string_view filterSchema =
            R"({"weatherInclusions":{"formIDs":[],"contains":[]},"weatherExclusions":{"formIDs":[],"contains":[]},"weatherPluginOwnership":{"exact":[],"contains":[]},"weatherPluginInclusions":{"exact":[],"contains":[]},"weatherPluginExclusions":{"exact":[],"contains":[]},"lightingTemplateInclusions":[],"lightingTemplateExclusions":[],"lightingTemplatePluginOwnership":{"exact":[],"contains":[]},"lightingTemplatePluginInclusions":{"exact":[],"contains":[]},"lightingTemplatePluginExclusions":{"exact":[],"contains":[]}})";
        std::string normalizationError;
        const auto normalized = JsonOverlay::Overlay(filterSchema, a_json, normalizationError);
        if (!normalized)
        {
            logger::warn("[TuningUtil] settings normalize failed | source={} | {}", a_source.string(), normalizationError);
            return std::nullopt;
        }
        const auto parsed = rfl::json::read<Settings, rfl::DefaultIfMissing>(*normalized);
        if (!parsed)
        {
            logger::warn("[TuningUtil] settings load failed | source={} | {}", a_source.string(), parsed.error().what());
            return std::nullopt;
        }
        auto settings = parsed.value();
        if (settings.normalizeLuminance)
        {
            const auto& luminance = *settings.normalizeLuminance;
            const std::array targets{ luminance.ambient, luminance.directional, luminance.ambientColors,
                luminance.fogFar, luminance.fogNear };
            if (std::ranges::any_of(targets, [](const double a_value)
                { return !std::isfinite(a_value) || a_value < 0.0 || a_value > 255.0; }))
            {
                logger::warn("[Lighting] settings load failed | source={} | normalizeLuminance targets must be between 0 and 255",
                    a_source.string());
                return std::nullopt;
            }
        }
        ConstrainBetweenWeatherCompression(settings.betweenWeatherCompression);
        return settings;
    }

}
