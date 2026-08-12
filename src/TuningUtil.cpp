#include <CSTonemapping.h>
#include <DetailedLogging.h>
#include <Config/Common.h>
#include <ImageSpacePatcher.h>
#include <JsonOverlay.h>
#include <LightingPatcher.h>
#include <TuningSettings.h>
#include <SliderSettingCatalog.h>
#include <TuningUtil.h>
#include <WeatherPatcher.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <regex>
#include <unordered_map>
#include <yyjson.h>

namespace MPL::TuningUtil
{
    namespace
    {
        const std::filesystem::path kProfileRoot{ "./Data/Luma/Tuning" };
        const std::filesystem::path kUserRoot{ "./Data/SKSE/Plugins/Luma" };
        const std::filesystem::path kGlobalDefaultsPath = kProfileRoot / "defaultSettings.json";
        constexpr std::string_view kProfileDefaultsFile = "profileSettings.json";
        constexpr std::string_view kMenuDefinitionFile = "skseMenu.json";

        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const { yyjson_doc_free(a_document); }
        };

        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;

        bool discoveryInitialized = false;
        bool pluginDependencyFilterReady = false;
        bool runtimeStateReleased = false;
        std::uint64_t settingsRevision = 0;
        std::vector<Profile> profiles;
        std::vector<std::filesystem::path> pluginFilteredProfileDirectories;
        struct CachedSettings
        {
            Settings settings;
            std::string localDefaults{ "{}" };
            std::string presetDefaults{ "{}" };
            std::string explicitUserSettings{ "{}" };
            std::optional<std::string> presetPreviewUserLayer;
        };
        std::unordered_map<std::string, CachedSettings> settingsCache;
        std::optional<std::string> globalDefaultsCache;
        struct SettingOwnershipLayers
        {
            std::array<std::vector<std::string>, 3> paths;
        };
        std::unordered_map<std::string, SettingOwnershipLayers> settingOwnershipCache;
        std::unordered_map<std::string, bool> startupSettingTargetOverlapCache;
        bool startupSettingTargetOverlapsCaptured = false;
        std::uint64_t settingOwnershipRevision = std::numeric_limits<std::uint64_t>::max();

        std::string SerializeSettings(const Settings& a_settings)
        {
            return rfl::json::write<rfl::NoOptionals>(
                a_settings,
                rfl::json::pretty);
        }

        std::string Lowercase(std::string a_value)
        {
            std::ranges::transform(a_value, a_value.begin(), [](const unsigned char a_character)
                { return static_cast<char>(std::tolower(a_character)); });
            return a_value;
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

        std::string ProfileName(std::string a_name)
        {
            a_name = std::filesystem::path(std::move(a_name)).filename().string();
            if (a_name.empty() || std::ranges::all_of(a_name, [](const unsigned char a_character)
                                     { return std::isspace(a_character) != 0; }))
            {
                return {};
            }
            const auto extension = std::filesystem::path(a_name).extension().string();
            if (Config::IEquals(extension, ".esp") || Config::IEquals(extension, ".esm") || Config::IEquals(extension, ".esl"))
            {
                a_name = std::filesystem::path(a_name).stem().string();
            }
            return a_name;
        }

        std::optional<std::string> ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            std::string text(std::istreambuf_iterator<char>(file), {});
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom))
            {
                text.erase(0, bom.size());
            }
            return text;
        }

        Document Parse(const std::string_view a_json)
        {
            return Document(yyjson_read(
                const_cast<char*>(a_json.data()),
                a_json.size(),
                YYJSON_READ_NOFLAG));
        }

        std::optional<std::string> JsonString(yyjson_val* a_object, const std::string_view a_key)
        {
            auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
            return yyjson_is_str(value) ?
                       std::optional<std::string>{ std::string(yyjson_get_str(value), yyjson_get_len(value)) } :
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

        std::optional<FilteredWeatherSetting> ParseFilteredWeatherSetting(const std::string_view a_setting)
        {
            const auto* entry = SliderSettingCatalog::Find(a_setting);
            if (!entry || entry->domain != SliderSettingCatalog::Domain::weather ||
                !SliderSettingCatalog::IsFilteredOperation(entry->filterOperation))
                return std::nullopt;
            FilteredWeatherSetting result;
            result.target = Lowercase(entry->target);
            switch (entry->filterOperation)
            {
            case SliderSettingCatalog::FilterOperation::brightness:
                result.operation = FilteredWeatherOperation::brightness;
                break;
            case SliderSettingCatalog::FilterOperation::saturation:
                result.operation = FilteredWeatherOperation::saturation;
                break;
            case SliderSettingCatalog::FilterOperation::hueShift:
                result.operation = FilteredWeatherOperation::hueShift;
                if (!entry->hue.empty()) result.hue = Lowercase(entry->hue);
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
            if (a_setting.starts_with("intBrightnessMultiplier.") && !entry->target.empty())
            {
                return FilteredLightingTemplateSetting{
                    .operation = FilteredLightingTemplateOperation::brightness,
                    .target = Lowercase(entry->target),
                };
            }
            if (a_setting == "intFogMaxMultiplier")
            {
                return FilteredLightingTemplateSetting{
                    .operation = FilteredLightingTemplateOperation::fogStrength,
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

        struct SliderSettingSpec
        {
            std::string path;
            double scale = 1.0;
            bool ignoreLink = true;
            bool structured = false;
        };

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
                        result.push_back({ std::move(path), 1.0, true, false });
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
                auto* ignoreLinkValue = yyjson_obj_get(a_value, "ignoreLink");
                const bool ignoreLink = yyjson_is_bool(ignoreLinkValue) && yyjson_get_bool(ignoreLinkValue);
                if (!std::ranges::any_of(result, [&](const auto& a_existing)
                        { return Config::IEquals(a_existing.path, *path); }))
                    result.push_back({ std::move(*path), scale, ignoreLink, true });
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

        bool HasStructuredSliderSettings(yyjson_val* a_control)
        {
            auto* settings = yyjson_obj_get(a_control, "settings");
            if (!yyjson_is_arr(settings)) return yyjson_is_obj(settings);
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(settings, index, maximum, item)
            {
                if (yyjson_is_obj(item)) return true;
            }
            return false;
        }

        bool IsInteriorLinkableSliderSetting(const std::string_view a_setting)
        {
            return a_setting.starts_with("intBrightnessMultiplier.") ||
                   a_setting.starts_with("intSaturationMultiplier.") ||
                   a_setting.starts_with("intHueShift.");
        }

        bool HasDirectInteriorLinkOverride(yyjson_val* a_control)
        {
            const auto settings = JsonSliderSettings(a_control);
            return !settings.empty() &&
                   std::ranges::all_of(settings, [](const auto& a_setting)
                       { return IsInteriorLinkableSliderSetting(a_setting.path); }) &&
                   std::ranges::any_of(settings, [](const auto& a_setting)
                       { return a_setting.structured && a_setting.ignoreLink; });
        }

        bool ValidSliderID(const std::string_view a_id)
        {
            return !a_id.empty() && std::ranges::all_of(a_id, [](const unsigned char a_character)
            {
                return std::isalnum(a_character) != 0 || a_character == '_' || a_character == '-';
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
            if (auto localLink = JsonString(a_control, "localLink"))
            {
                *localLink = Lowercase(Trim(std::move(*localLink)));
                if (!localLink->empty()) rule.localLink = std::move(*localLink);
            }
            rule.hueScales = JsonHueScales(a_control);
            for (const auto& specification : a_settings)
            {
                auto setting = ParseFilteredWeatherSetting(specification.path);
                if (!setting)
                {
                    logger::warn("[TuningUtil] filtered slider={} | source={} | setting={} unsupported", rule.id, a_source.string(), specification.path);
                    return std::nullopt;
                }
                if (!rule.settings.empty() && setting->operation != rule.settings.front().operation)
                {
                    logger::warn("[TuningUtil] filtered slider={} | source={} | grouped operations conflict", rule.id, a_source.string());
                    return std::nullopt;
                }
                setting->scale = specification.scale;
                setting->ignoreLink = specification.ignoreLink;
                rule.settings.push_back(std::move(*setting));
            }
            if (rule.settings.empty() || !ParseTimes(a_control, rule.times, rule.id, a_source))
            {
                return std::nullopt;
            }
            if (rule.localLink)
            {
                const auto operation = rule.settings.front().operation;
                const auto valid = std::ranges::any_of(
                    SliderSettingCatalog::Entries(),
                    [&](const auto& a_entry)
                    {
                        if (a_entry.domain != SliderSettingCatalog::Domain::weather ||
                            !Config::IEquals(a_entry.target, *rule.localLink))
                            return false;
                        switch (operation)
                        {
                        case FilteredWeatherOperation::brightness:
                            return a_entry.filterOperation == SliderSettingCatalog::FilterOperation::brightness;
                        case FilteredWeatherOperation::saturation:
                            return a_entry.filterOperation == SliderSettingCatalog::FilterOperation::saturation;
                        case FilteredWeatherOperation::hueShift:
                            return a_entry.filterOperation == SliderSettingCatalog::FilterOperation::hueShift;
                        }
                        return false;
                    });
                if (!valid)
                {
                    logger::warn("[TuningUtil] filtered slider={} | source={} | local link={} unsupported",
                        rule.id, a_source.string(), *rule.localLink);
                    return std::nullopt;
                }
            }
            if (rule.hueScales && rule.settings.front().operation != FilteredWeatherOperation::saturation)
            {
                logger::warn("[TuningUtil] filtered slider={} | source={} | saturation settings required",
                    rule.id, a_source.string());
                return std::nullopt;
            }
            rule.defaultValue = rule.settings.front().operation == FilteredWeatherOperation::hueShift && !rule.localLink ? 0.0 : 1.0;
            if (auto* value = yyjson_obj_get(a_control, "default"); yyjson_is_num(value))
            {
                rule.defaultValue = yyjson_get_real(value);
            }
            rule.include = JsonWeatherFilter(yyjson_obj_get(a_control, "weatherFilter"), "include");
            rule.exclude = JsonWeatherFilter(yyjson_obj_get(a_control, "weatherFilter"), "exclude");
            return rule;
        }

        std::vector<FilteredWeatherRule> ParseFilteredWeatherRules(
            yyjson_val* a_control,
            const std::filesystem::path& a_source)
        {
            std::vector<FilteredWeatherRule> result;
            if (yyjson_is_obj(yyjson_obj_get(a_control, "lightingTemplateFilter"))) return result;
            const auto kind = Lowercase(Trim(JsonString(a_control, "type").value_or("")));
            const auto times = JsonStrings(a_control, "times");
            const auto filtered = !times.empty() || yyjson_is_obj(yyjson_obj_get(a_control, "weatherFilter")) ||
                                   yyjson_is_str(yyjson_obj_get(a_control, "localLink")) ||
                                   yyjson_is_obj(yyjson_obj_get(a_control, "hueScales")) ||
                                   (HasStructuredSliderSettings(a_control) &&
                                       !HasDirectInteriorLinkOverride(a_control));
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
                    specifications.push_back({ std::move(setting), 1.0, true });
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
                for (const auto target : targets | std::views::take(15))
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
            };
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
                setting->ignoreLink = specification.structured && specification.ignoreLink;
                rule.settings.push_back(std::move(*setting));
            }
            if (rule.settings.empty())
            {
                logger::warn(
                    "[TuningUtil] filtered Lighting Template slider={} | source={} | settings unsupported",
                    rule.id,
                    a_source.string());
                return std::nullopt;
            }
            if (auto* value = yyjson_obj_get(a_control, "default"); yyjson_is_num(value))
            {
                const auto candidate = yyjson_get_real(value);
                if (std::isfinite(candidate)) rule.defaultValue = candidate;
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

        std::vector<FilteredWeatherRule> ReadFilteredWeatherRules(const std::filesystem::path& a_profileDirectory)
        {
            const auto path = a_profileDirectory / kMenuDefinitionFile;
            const auto text = ReadText(path);
            const auto document = text ? Parse(*text) : nullptr;
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (!yyjson_is_obj(root))
            {
                return {};
            }
            auto* schemaVersion = yyjson_obj_get(root, "schemaVersion");
            if (!yyjson_is_num(schemaVersion) || yyjson_get_num(schemaVersion) != 1.0)
            {
                return {};
            }

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
            const std::filesystem::path& a_profileDirectory)
        {
            const auto path = a_profileDirectory / kMenuDefinitionFile;
            const auto text = ReadText(path);
            const auto document = text ? Parse(*text) : nullptr;
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (!yyjson_is_obj(root)) return {};
            auto* schemaVersion = yyjson_obj_get(root, "schemaVersion");
            if (!yyjson_is_num(schemaVersion) || yyjson_get_num(schemaVersion) != 1.0) return {};

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

        struct InteriorSliderLinkRules
        {
            std::vector<std::string> settings;
            std::vector<std::string> ignoredLinks;
        };

        InteriorSliderLinkRules ReadInteriorSliderLinkRules(
            const std::filesystem::path& a_profileDirectory)
        {
            const auto path = a_profileDirectory / kMenuDefinitionFile;
            const auto text = ReadText(path);
            const auto document = text ? Parse(*text) : nullptr;
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (!yyjson_is_obj(root)) return {};
            auto* schemaVersion = yyjson_obj_get(root, "schemaVersion");
            if (!yyjson_is_num(schemaVersion) || yyjson_get_num(schemaVersion) != 1.0) return {};

            InteriorSliderLinkRules result;
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
                    if (!Config::IEquals(Trim(JsonString(control, "type").value_or("")), "slider") ||
                        yyjson_is_obj(yyjson_obj_get(control, "weatherFilter")) ||
                        yyjson_is_obj(yyjson_obj_get(control, "lightingTemplateFilter")))
                        continue;

                    for (const auto& specification : JsonSliderSettings(control))
                    {
                        if (!IsInteriorLinkableSliderSetting(specification.path) ||
                            !SliderSettingCatalog::Find(specification.path))
                            continue;
                        add(result.settings, specification.path);
                        if (specification.structured && specification.ignoreLink)
                            add(result.ignoredLinks, specification.path);
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
            return result;
        }

        std::string FilteredDefaultsText(const Profile& a_profile)
        {
            struct Defaults
            {
                std::map<std::string, double> filteredWeatherAdjustments;
                std::map<std::string, double> filteredLightingTemplateAdjustments;
            } defaults;
            for (const auto& rule : a_profile.filteredWeatherRules)
            {
                defaults.filteredWeatherAdjustments.emplace(rule.id, rule.defaultValue);
            }
            for (const auto& rule : a_profile.filteredLightingTemplateRules)
            {
                defaults.filteredLightingTemplateAdjustments.emplace(rule.id, rule.defaultValue);
            }
            return rfl::json::write(defaults, rfl::json::pretty);
        }

        std::optional<std::string> StringMember(
            const std::string_view a_json,
            const std::string_view a_name)
        {
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            auto* value = yyjson_is_obj(root) ? yyjson_obj_getn(root, a_name.data(), a_name.size()) : nullptr;
            return yyjson_is_str(value) ?
                       std::optional<std::string>{ std::string(yyjson_get_str(value), yyjson_get_len(value)) } :
                       std::nullopt;
        }

        std::optional<int> IntegerMember(
            const std::string_view a_json,
            const std::string_view a_name)
        {
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            auto* value = yyjson_is_obj(root) ? yyjson_obj_getn(root, a_name.data(), a_name.size()) : nullptr;
            if (!yyjson_is_int(value) && !yyjson_is_uint(value))
            {
                return std::nullopt;
            }
            const auto priority = yyjson_get_sint(value);
            return priority >= std::numeric_limits<int>::min() && priority <= std::numeric_limits<int>::max() ?
                       std::optional<int>{ static_cast<int>(priority) } :
                       std::nullopt;
        }

        bool AnyPluginLoaded(const std::span<const std::string> a_plugins)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler && std::ranges::any_of(a_plugins, [&](const auto& a_plugin)
            {
                return dataHandler->LookupLoadedModByName(a_plugin) ||
                       dataHandler->LookupLoadedLightModByName(a_plugin);
            });
        }

        bool PluginDependenciesSatisfied(const std::span<const std::string> a_dependencies)
        {
            return a_dependencies.empty() || AnyPluginLoaded(a_dependencies);
        }

        std::string PluginDependencyList(const std::span<const std::string> a_dependencies)
        {
            std::string result;
            for (const auto& dependency : a_dependencies)
            {
                if (!result.empty())
                {
                    result.append(", ");
                }
                result.append(dependency);
            }
            return result;
        }

        std::vector<std::string> SettingRoots(const std::string_view a_json)
        {
            std::vector<std::string> roots;
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (!yyjson_is_obj(root))
            {
                return roots;
            }
            yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                roots.emplace_back(yyjson_get_str(key), yyjson_get_len(key));
            }
            return roots;
        }

        void AddSettingPath(std::vector<std::string>& a_paths, const std::string_view a_path)
        {
            if (!a_path.empty() && !std::ranges::contains(a_paths, a_path))
            {
                a_paths.emplace_back(a_path);
            }
        }

        void AddExpandedSettingPaths(std::vector<std::string>& a_paths, const std::string_view a_setting)
        {
            if (a_setting == "brightness")
            {
                AddSettingPath(a_paths, "brightnessMultiplier");
                AddSettingPath(a_paths, "volumetricLightingIntensityMultiplier");
            }
            else if (a_setting == "saturation") AddSettingPath(a_paths, "saturationMultiplier");
            else if (a_setting == "betweenCompression") AddSettingPath(a_paths, "betweenWeatherCompression");
            else if (a_setting == "withinCompression") AddSettingPath(a_paths, "withinWeatherCompression");
            else if (a_setting == "intBrightness")
            {
                AddSettingPath(a_paths, "intBrightnessMultiplier");
                AddSettingPath(a_paths, "intFogMaxMultiplier");
            }
            else if (a_setting == "intSaturation") AddSettingPath(a_paths, "intSaturationMultiplier");
            else if (a_setting == "intHueScales") AddSettingPath(a_paths, "intAmbientHueScales");
            else if (a_setting == "intHueRanges") AddSettingPath(a_paths, "intHueRanges");
            else AddSettingPath(a_paths, a_setting);
        }

        const Profile* FindProfile(const std::string& a_name)
        {
            const auto name = ProfileName(a_name);
            const auto& discovered = GetProfiles();
            const auto match = std::ranges::find_if(discovered, [&](const Profile& a_profile)
                { return Config::IEquals(a_profile.name, name); });
            return match != discovered.end() ? std::addressof(*match) : nullptr;
        }

        std::filesystem::path UserSettingsPath(const Profile& a_profile)
        {
            return kUserRoot / a_profile.directory.filename() / "userSettings.json";
        }

        std::filesystem::path ProfileDefaultsPath(const Profile& a_profile)
        {
            return a_profile.directory / kProfileDefaultsFile;
        }

        std::optional<Settings> ParseSettings(const std::string& a_json, const std::filesystem::path& a_source)
        {
            static constexpr std::string_view filterSchema =
                R"({"weatherInclusions":{"formIDs":[],"contains":[]},"weatherExclusions":{"formIDs":[],"contains":[]},"pluginInclusions":{"exact":[],"contains":[]},"pluginExclusions":{"exact":[],"contains":[]},"effectLightingInclusions":{"formIDs":[],"contains":[]},"effectLightingExclusions":{"formIDs":[],"contains":[]},"effectLightingPluginInclusions":{"exact":[],"contains":[]},"effectLightingPluginExclusions":{"exact":[],"contains":[]},"lightingTemplateInclusions":[],"lightingTemplateExclusions":[],"lightingTemplatePluginInclusions":{"exact":[],"contains":[]},"lightingTemplatePluginExclusions":{"exact":[],"contains":[]},"pointLightEffectLightingExclusions":[]})";
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
            return parsed.value();
        }

        const std::string& GlobalDefaultsText()
        {
            if (globalDefaultsCache)
            {
                return *globalDefaultsCache;
            }
            if (const auto text = ReadText(kGlobalDefaultsPath); text && ParseSettings(*text, kGlobalDefaultsPath))
            {
                globalDefaultsCache = *text;
                return *globalDefaultsCache;
            }

            logger::warn(
                "[TuningUtil] global defaults invalid | source={} | fallback=compiled",
                kGlobalDefaultsPath.string());
            globalDefaultsCache = SerializeSettings(Settings{});
            return *globalDefaultsCache;
        }

        std::optional<std::string> LocalDefaultsText(const Profile& a_profile, std::string& a_error)
        {
            const auto profileDefaults = ReadText(ProfileDefaultsPath(a_profile));
            const auto local = profileDefaults ?
                                   JsonOverlay::Overlay(GlobalDefaultsText(), *profileDefaults, a_error) :
                                   std::nullopt;
            return local ?
                       JsonOverlay::Overlay(*local, FilteredDefaultsText(a_profile), a_error) :
                       std::nullopt;
        }

        std::optional<std::string> ActivePresetSettingsText(
            const Profile& a_profile,
            std::string& a_error)
        {
            auto profileName = a_profile.name;
            const auto active = WeatherPatcher::GetActivePresetSettings(profileName, a_error);
            const auto schema = active ? LocalDefaultsText(a_profile, a_error) : std::nullopt;
            return active && schema ? JsonOverlay::ProjectLike(*active, *schema, a_error) : std::nullopt;
        }

        std::optional<std::string> PresetDefaultsText(
            const Profile& a_profile,
            std::string& a_error)
        {
            const auto localDefaults = LocalDefaultsText(a_profile, a_error);
            const auto activePresets = localDefaults ? ActivePresetSettingsText(a_profile, a_error) : std::nullopt;
            return localDefaults && activePresets ?
                       JsonOverlay::Overlay(*localDefaults, *activePresets, a_error) :
                       std::nullopt;
        }

        std::optional<std::string> StoredSettingsText(const Profile&, std::string_view, std::string&);

        std::optional<CachedSettings> LoadStoredSettings(const Profile& a_profile)
        {
            std::string error;
            const auto localDefaults = LocalDefaultsText(a_profile, error);
            if (!localDefaults)
            {
                logger::warn("[TuningUtil] {} defaults compose failed | {}", a_profile.name, error);
                return std::nullopt;
            }
            const auto activePresetSettings = ActivePresetSettingsText(a_profile, error);
            const auto presetDefaults = activePresetSettings ?
                                            JsonOverlay::Overlay(*localDefaults, *activePresetSettings, error) :
                                            std::nullopt;
            auto defaults = presetDefaults ? ParseSettings(*presetDefaults, ProfileDefaultsPath(a_profile)) : std::nullopt;
            if (!defaults)
            {
                logger::warn("[TuningUtil] {} preset compose failed | {}", a_profile.name, error);
                return std::nullopt;
            }

            auto settings = *defaults;
            const auto stored = StoredSettingsText(a_profile, *presetDefaults, error);
            std::string explicitUserSettings{ "{}" };
            if (stored && std::filesystem::is_regular_file(UserSettingsPath(a_profile)))
            {
                if (auto parsed = ParseSettings(*stored, UserSettingsPath(a_profile)))
                {
                    settings = std::move(*parsed);
                    explicitUserSettings = ReadText(UserSettingsPath(a_profile)).value_or("{}");
                    DetailedLogging::Info("[TuningUtil] {} user overrides | source={}", a_profile.name, UserSettingsPath(a_profile).string());
                }
            }
            else if (!stored)
            {
                logger::warn("[TuningUtil] user settings merge failed | source={} | {}", UserSettingsPath(a_profile).string(), error);
            }
            return CachedSettings{
                std::move(settings),
                *localDefaults,
                *presetDefaults,
                std::move(explicitUserSettings) };
        }

        std::string CompactLinkArrays(const std::string& a_json)
        {
            static const std::regex arrays(
                R"json(\[\s*("(?:\\.|[^"\\])*")\s*,\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)\s*\])json");
            return std::regex_replace(a_json, arrays, "[ $1, $2 ]");
        }

        std::vector<std::string> ExpandSettingScopes(const std::span<const std::string> a_scopes)
        {
            std::vector<std::string> paths;
            for (const auto& scope : a_scopes)
            {
                AddExpandedSettingPaths(paths, scope);
            }
            return paths;
        }

        std::optional<std::string> CombineDeclaredSettings(
            const std::string_view a_source,
            const std::string_view a_defaults,
            std::string& a_error)
        {
            return JsonOverlay::ProjectLike(a_source, a_defaults, a_error);
        }

        std::optional<std::string> StoredSettingsText(
            const Profile& a_profile,
            const std::string_view a_defaults,
            std::string& a_error)
        {
            const auto user = ReadText(UserSettingsPath(a_profile));
            if (!user)
            {
                return std::string(a_defaults);
            }
            return JsonOverlay::Merge(a_defaults, *user, a_error);
        }

        bool ApplySettingsPatch(
            const Profile& a_profile,
            std::string& a_profileName,
            const std::string_view a_patch)
        {
            const auto patch = Parse(a_patch);
            auto* patchRoot = patch ? yyjson_doc_get_root(patch.get()) : nullptr;
            if (!yyjson_is_obj(patchRoot))
            {
                return false;
            }
            if (yyjson_obj_size(patchRoot) == 0)
            {
                return true;
            }

            std::string error;
            const auto current = SerializeSettings(GetSettings(a_profileName));
            const auto overlaid = JsonOverlay::Overlay(current, a_patch, error);
            auto parsed = overlaid ? ParseSettings(*overlaid, a_profile.directory / "runtime settings") : std::nullopt;
            if (!parsed)
            {
                logger::warn("[TuningUtil] {} settings patch failed | {}", a_profileName, error);
                return false;
            }
            GetSettings(a_profileName) = std::move(*parsed);
            ApplySettings();
            return true;
        }

        bool WriteUserSettings(
            const Profile& a_profile,
            const std::string_view a_settings)
        {
            const auto path = UserSettingsPath(a_profile);
            std::error_code fileError;
            std::filesystem::create_directories(path.parent_path(), fileError);
            if (fileError)
            {
                logger::warn("[TuningUtil] user directory create failed | path={} | {}", path.parent_path().string(), fileError.message());
                return false;
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << CompactLinkArrays(std::string(a_settings)) << '\n';
            if (!file)
            {
                logger::warn("[TuningUtil] user settings save failed | path={}", path.string());
                return false;
            }
            logger::info("[TuningUtil] user overrides | path={} | status=saved", path.string());
            return true;
        }

        bool WriteTextAtomically(
            const std::filesystem::path& a_path,
            const std::string_view a_text,
            std::string& a_error)
        {
            auto temporaryPath = a_path;
            temporaryPath += ".tmp";
            {
                std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
                file << a_text << '\n';
                if (!file)
                {
                    file.close();
                    std::error_code removeError;
                    std::filesystem::remove(temporaryPath, removeError);
                    a_error = std::format("The temporary profile settings file could not be written: {}", temporaryPath.string());
                    return false;
                }
            }
            if (::MoveFileExW(
                    temporaryPath.c_str(),
                    a_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;

            const std::error_code moveError(static_cast<int>(::GetLastError()), std::system_category());
            std::error_code removeError;
            std::filesystem::remove(temporaryPath, removeError);
            a_error = std::format("The profile settings file could not be replaced: {}", moveError.message());
            return false;
        }

        bool WriteSettings(
            const Profile& a_profile,
            const std::string& a_profileName,
            const Settings& a_settings)
        {
            std::string error;
            const auto defaultText = PresetDefaultsText(a_profile, error);
            if (!defaultText)
            {
                return false;
            }
            if (!ParseSettings(*defaultText, ProfileDefaultsPath(a_profile)))
            {
                logger::warn("[TuningUtil] {} save rejected | defaults unreadable", a_profileName);
                return false;
            }
            const auto current = SerializeSettings(a_settings);
            const auto declared = CombineDeclaredSettings(current, *defaultText, error);
            if (!declared)
            {
                logger::warn("[TuningUtil] {} save failed | declared settings | {}", a_profileName, error);
                return false;
            }
            const auto difference = JsonOverlay::Difference(*declared, *defaultText, error);
            if (!difference)
            {
                logger::warn("[TuningUtil] {} save failed | sparse settings | {}", a_profileName, error);
                return false;
            }
            if (!WriteUserSettings(a_profile, *difference))
            {
                return false;
            }
            if (auto cached = settingsCache.find(Lowercase(a_profile.name)); cached != settingsCache.end())
            {
                cached->second.explicitUserSettings = *difference;
                if (cached->second.presetPreviewUserLayer)
                {
                    cached->second.presetPreviewUserLayer = *difference;
                }
            }
            return true;
        }

        std::optional<std::string> CurrentUserLayer(const Profile& a_profile, std::string& a_error)
        {
            auto profileName = a_profile.name;
            const auto current = SerializeSettings(GetSettings(profileName));
            const auto cached = settingsCache.find(Lowercase(a_profile.name));
            if (cached == settingsCache.end())
            {
                a_error = "The profile settings cache is unavailable.";
                return std::nullopt;
            }

            const auto explicitValues = JsonOverlay::ProjectLike(
                current,
                cached->second.explicitUserSettings,
                a_error);
            const auto changedValues = JsonOverlay::Difference(
                current,
                cached->second.presetDefaults,
                a_error);
            return explicitValues && changedValues ?
                       JsonOverlay::Overlay(*explicitValues, *changedValues, a_error) :
                       std::nullopt;
        }

        bool HasAnyRoot(
            const std::span<const std::string> a_declaredRoots,
            const std::span<const std::string_view> a_requestedRoots)
        {
            return std::ranges::any_of(a_requestedRoots, [&](const auto requested)
            {
                return std::ranges::any_of(a_declaredRoots, [&](const auto& declared)
                    { return declared == requested; });
            });
        }

        bool OwnsAnySetting(
            const Profile& a_profile,
            const std::span<const std::string_view> a_requestedRoots)
        {
            if (HasAnyRoot(a_profile.defaultSettingRoots, a_requestedRoots))
            {
                return true;
            }
            std::string error;
            auto profileName = a_profile.name;
            const auto activePresets = WeatherPatcher::GetActivePresetSettings(profileName, error);
            if (activePresets && HasAnyRoot(SettingRoots(*activePresets), a_requestedRoots))
            {
                return true;
            }
            const auto userLayer = CurrentUserLayer(a_profile, error);
            return userLayer && HasAnyRoot(SettingRoots(*userLayer), a_requestedRoots);
        }

        void CollectSettingPaths(
            yyjson_val* a_value,
            const std::string_view a_prefix,
            std::vector<std::string>& a_paths)
        {
            if (!yyjson_is_obj(a_value))
            {
                AddSettingPath(a_paths, a_prefix);
                return;
            }

            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_value);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
                const auto path = a_prefix.empty() ? std::string(name) : std::string(a_prefix) + "." + std::string(name);
                CollectSettingPaths(yyjson_obj_iter_get_val(key), path, a_paths);
            }
        }

        std::vector<std::string> SettingPaths(const std::string_view a_json)
        {
            std::vector<std::string> paths;
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (yyjson_is_obj(root))
            {
                CollectSettingPaths(root, {}, paths);
            }
            return paths;
        }

        bool SettingPathsOverlap(const std::string_view a_left, const std::string_view a_right)
        {
            return a_left == a_right ||
                   (a_left.size() > a_right.size() && a_left.starts_with(a_right) && a_left[a_right.size()] == '.') ||
                   (a_right.size() > a_left.size() && a_right.starts_with(a_left) && a_right[a_left.size()] == '.');
        }

        bool IsProfileLocalSettingPath(const std::string_view a_path)
        {
            static constexpr std::array roots{
                std::string_view{ "profile" },
                std::string_view{ "profilePriority" },
                std::string_view{ "PluginDependency" },
                std::string_view{ "PluginIndependency" },
                std::string_view{ "EnableProfile" },
                std::string_view{ "ShowAdvanced" },
                std::string_view{ "weatherInclusions" },
                std::string_view{ "weatherExclusions" },
                std::string_view{ "pluginInclusions" },
                std::string_view{ "pluginExclusions" },
                std::string_view{ "effectLightingInclusions" },
                std::string_view{ "effectLightingExclusions" },
                std::string_view{ "effectLightingPluginInclusions" },
                std::string_view{ "effectLightingPluginExclusions" },
                std::string_view{ "lightingTemplateInclusions" },
                std::string_view{ "lightingTemplateExclusions" },
                std::string_view{ "lightingTemplatePluginInclusions" },
                std::string_view{ "lightingTemplatePluginExclusions" },
                std::string_view{ "enableTemplateInherit" },
                std::string_view{ "cellExclusions" },
                std::string_view{ "pointLightEffectLightingExclusions" },
            };
            return std::ranges::any_of(roots, [&](const auto root)
                { return a_path == root || (a_path.starts_with(root) && a_path.size() > root.size() && a_path[root.size()] == '.'); });
        }

        enum class SettingFilterDomain
        {
            unfiltered,
            weather,
            filteredWeather,
            effectLighting,
            interior,
            filteredLightingTemplate,
        };

        SettingFilterDomain FilterDomainForSetting(const std::string_view a_path)
        {
            if (a_path.starts_with("filteredWeatherAdjustments."))
            {
                return SettingFilterDomain::filteredWeather;
            }
            if (a_path.starts_with("filteredLightingTemplateAdjustments."))
            {
                return SettingFilterDomain::filteredLightingTemplate;
            }
            if (a_path == "fxEffectLighting" || a_path.starts_with("fxEffectLighting."))
            {
                return SettingFilterDomain::effectLighting;
            }
            if (a_path == "links.interior" || a_path.starts_with("links.interior.") ||
                a_path == "intBrightnessMultiplier" || a_path.starts_with("intBrightnessMultiplier.") ||
                a_path == "intSaturationMultiplier" || a_path.starts_with("intSaturationMultiplier.") ||
                a_path == "intHueShift" || a_path.starts_with("intHueShift.") ||
                a_path == "intAmbientHueScales" || a_path.starts_with("intAmbientHueScales.") ||
                a_path == "intFogMaxMultiplier")
            {
                return SettingFilterDomain::interior;
            }
            if (a_path == "links.weather" || a_path.starts_with("links.weather.") ||
                a_path == "brightnessMultiplier" || a_path.starts_with("brightnessMultiplier.") ||
                a_path == "volumetricLightingIntensityMultiplier" ||
                a_path == "saturationMultiplier" || a_path.starts_with("saturationMultiplier.") ||
                a_path == "hueScales" || a_path.starts_with("hueScales.") ||
                a_path == "hueRanges" || a_path.starts_with("hueRanges.") ||
                a_path == "hueShift" || a_path.starts_with("hueShift.") ||
                a_path == "betweenWeatherCompression" || a_path.starts_with("betweenWeatherCompression.") ||
                a_path == "withinWeatherCompression" || a_path.starts_with("withinWeatherCompression.") ||
                a_path == "compressionAnchor" || a_path.starts_with("compressionAnchor.") ||
                a_path == "dynamicAmbientWithin" || a_path.starts_with("dynamicAmbientWithin.") ||
                a_path == "dynamicAmbientBetween" || a_path.starts_with("dynamicAmbientBetween.") ||
                a_path == "dynamicSunlightWithin" || a_path.starts_with("dynamicSunlightWithin.") ||
                a_path == "dynamicSunlightBetween" || a_path.starts_with("dynamicSunlightBetween."))
            {
                return SettingFilterDomain::weather;
            }
            return SettingFilterDomain::unfiltered;
        }

        std::string SettingTargetOverlapKey(
            const std::string& a_leftProfile,
            const std::string& a_rightProfile,
            const std::string_view a_settingPath)
        {
            const auto domain = FilterDomainForSetting(a_settingPath);
            auto leftKey = Lowercase(a_leftProfile);
            auto rightKey = Lowercase(a_rightProfile);
            if (rightKey < leftKey) std::swap(leftKey, rightKey);
            return std::format(
                "{}\x1F{}\x1F{}\x1F{}",
                leftKey,
                rightKey,
                static_cast<int>(domain),
                domain == SettingFilterDomain::filteredWeather ||
                        domain == SettingFilterDomain::filteredLightingTemplate ?
                    a_settingPath :
                    std::string_view{});
        }

        bool ComputeProfilesShareSettingTargets(
            const std::string& a_leftProfile,
            const std::string& a_rightProfile,
            const std::string_view a_settingPath)
        {
            const auto domain = FilterDomainForSetting(a_settingPath);
            if (domain == SettingFilterDomain::unfiltered || !RE::TESDataHandler::GetSingleton())
            {
                return true;
            }

            bool sharesTarget = true;
            switch (domain)
            {
            case SettingFilterDomain::weather:
                sharesTarget = WeatherPatcher::ProfilesShareWeatherTarget(a_leftProfile, a_rightProfile);
                break;
            case SettingFilterDomain::filteredWeather:
                sharesTarget = WeatherPatcher::ProfilesShareFilteredWeatherTarget(
                    a_leftProfile,
                    a_rightProfile,
                    a_settingPath.substr(std::string_view("filteredWeatherAdjustments.").size()));
                break;
            case SettingFilterDomain::effectLighting:
                sharesTarget = WeatherPatcher::ProfilesShareEffectLightingTarget(a_leftProfile, a_rightProfile);
                break;
            case SettingFilterDomain::interior:
                sharesTarget = LightingPatcher::ProfilesShareInteriorTarget(a_leftProfile, a_rightProfile);
                break;
            case SettingFilterDomain::filteredLightingTemplate:
                sharesTarget = LightingPatcher::ProfilesShareFilteredLightingTemplateTarget(
                    a_leftProfile,
                    a_rightProfile,
                    a_settingPath.substr(std::string_view("filteredLightingTemplateAdjustments.").size()));
                break;
            case SettingFilterDomain::unfiltered:
                break;
            }
            return sharesTarget;
        }

        bool ProfilesShareSettingTargets(
            const std::string& a_leftProfile,
            const std::string& a_rightProfile,
            const std::string_view a_settingPath)
        {
            if (FilterDomainForSetting(a_settingPath) == SettingFilterDomain::unfiltered ||
                !startupSettingTargetOverlapsCaptured)
            {
                return true;
            }
            const auto cached = startupSettingTargetOverlapCache.find(
                SettingTargetOverlapKey(a_leftProfile, a_rightProfile, a_settingPath));
            return cached != startupSettingTargetOverlapCache.end() ? cached->second : true;
        }

        void CaptureStartupSettingTargetOverlaps()
        {
            startupSettingTargetOverlapCache.clear();
            if (!RE::TESDataHandler::GetSingleton())
            {
                startupSettingTargetOverlapsCaptured = true;
                return;
            }

            std::vector<std::string> filteredWeatherPaths;
            std::vector<std::string> filteredLightingTemplatePaths;
            for (const auto& profile : profiles)
            {
                for (const auto& rule : profile.filteredWeatherRules)
                {
                    AddSettingPath(filteredWeatherPaths, "filteredWeatherAdjustments." + rule.id);
                }
                for (const auto& rule : profile.filteredLightingTemplateRules)
                {
                    AddSettingPath(
                        filteredLightingTemplatePaths,
                        "filteredLightingTemplateAdjustments." + rule.id);
                }
            }

            static constexpr std::array representativePaths{
                std::string_view{ "brightnessMultiplier" },
                std::string_view{ "fxEffectLighting" },
                std::string_view{ "intBrightnessMultiplier" },
            };
            const auto cache = [&](const Profile& a_left, const Profile& a_right, const std::string_view a_path)
            {
                startupSettingTargetOverlapCache[SettingTargetOverlapKey(a_left.name, a_right.name, a_path)] =
                    ComputeProfilesShareSettingTargets(a_left.name, a_right.name, a_path);
            };
            for (std::size_t left = 0; left < profiles.size(); ++left)
            {
                for (std::size_t right = left + 1; right < profiles.size(); ++right)
                {
                    for (const auto path : representativePaths) cache(profiles[left], profiles[right], path);
                    for (const auto& path : filteredWeatherPaths) cache(profiles[left], profiles[right], path);
                    for (const auto& path : filteredLightingTemplatePaths) cache(profiles[left], profiles[right], path);
                }
            }
            startupSettingTargetOverlapsCaptured = true;
        }

        void RefreshSettingOwnershipCache()
        {
            if (settingOwnershipRevision == settingsRevision)
            {
                return;
            }

            settingOwnershipCache.clear();
            for (const auto& profile : profiles)
            {
                auto& layers = settingOwnershipCache[Lowercase(profile.name)].paths;
                layers[0] = SettingPaths(ReadText(ProfileDefaultsPath(profile)).value_or("{}"));

                std::string error;
                layers[1] = SettingPaths(ActivePresetSettingsText(profile, error).value_or("{}"));
                layers[2] = SettingPaths(CurrentUserLayer(profile, error).value_or("{}"));
            }
            settingOwnershipRevision = settingsRevision;
        }

        void SortProfiles()
        {
            std::ranges::sort(profiles, [](const Profile& a_left, const Profile& a_right)
            {
                if (a_left.priority != a_right.priority)
                {
                    return a_left.priority < a_right.priority;
                }
                const auto left = Lowercase(a_left.name);
                const auto right = Lowercase(a_right.name);
                return left != right ? left < right : a_left.name < a_right.name;
            });
        }

        void SynchronizeProfilePriorities()
        {
            for (auto& profile : profiles)
            {
                if (const auto cached = settingsCache.find(Lowercase(profile.name)); cached != settingsCache.end())
                {
                    profile.priority = cached->second.settings.profilePriority;
                }
            }
            SortProfiles();
        }
    }  // namespace

    void InvalidateDiscoveryCaches()
    {
        WeatherPatcher::InvalidatePresetCache();
        discoveryInitialized = false;
        profiles.clear();
        pluginFilteredProfileDirectories.clear();
        settingsCache.clear();
        settingOwnershipCache.clear();
        startupSettingTargetOverlapCache.clear();
        startupSettingTargetOverlapsCaptured = false;
        settingOwnershipRevision = std::numeric_limits<std::uint64_t>::max();
        globalDefaultsCache.reset();
    }

    const std::vector<Profile>& GetProfiles()
    {
        if (runtimeStateReleased)
        {
            return profiles;
        }
        if (discoveryInitialized)
        {
            return profiles;
        }

        std::error_code error;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator iterator(kProfileRoot, options, error), end;
            iterator != end && !error;
            iterator.increment(error))
        {
            if (!iterator->is_directory(error))
            {
                continue;
            }
            const auto defaultPath = iterator->path() / kProfileDefaultsFile;
            if (!std::filesystem::is_regular_file(defaultPath, error))
            {
                continue;
            }
            const auto defaults = ReadText(defaultPath);
            if (!defaults)
            {
                continue;
            }
            const auto name = ProfileName(StringMember(*defaults, "profile").value_or(""));
            if (name.empty())
            {
                logger::warn(
                    "[TuningUtil] profile ignored | path={} | name invalid",
                    iterator->path().string());
                continue;
            }
            if (!ParseSettings(*defaults, defaultPath))
            {
                logger::warn("[TuningUtil] {} ignored | defaults invalid", name);
                continue;
            }
            if (std::ranges::any_of(profiles, [&](const Profile& a_profile)
                    { return Config::IEquals(a_profile.name, name); }))
            {
                logger::warn("[TuningUtil] {} ignored | duplicate | path={}", name, iterator->path().string());
                continue;
            }
            const auto document = Parse(*defaults);
            const auto dependencies = JsonStrings(
                document ? yyjson_doc_get_root(document.get()) : nullptr,
                "PluginDependency");
            const auto independencies = JsonStrings(
                document ? yyjson_doc_get_root(document.get()) : nullptr,
                "PluginIndependency");
            if (pluginDependencyFilterReady && AnyPluginLoaded(independencies))
            {
                pluginFilteredProfileDirectories.push_back(iterator->path());
                logger::info(
                    "[TuningUtil] {} disabled | PluginIndependency={}",
                    name,
                    PluginDependencyList(independencies));
                continue;
            }
            if (pluginDependencyFilterReady && !PluginDependenciesSatisfied(dependencies))
            {
                pluginFilteredProfileDirectories.push_back(iterator->path());
                logger::info(
                    "[TuningUtil] {} disabled | PluginDependency missing={}",
                    name,
                    PluginDependencyList(dependencies));
                continue;
            }
            const auto user = ReadText(kUserRoot / iterator->path().filename() / "userSettings.json");
            auto priority = IntegerMember(GlobalDefaultsText(), "profilePriority").value_or(0);
            priority = IntegerMember(*defaults, "profilePriority").value_or(priority);
            if (user)
            {
                priority = IntegerMember(*user, "profilePriority").value_or(priority);
            }
            auto interiorSliderLinks = ReadInteriorSliderLinkRules(iterator->path());
            profiles.push_back({
                .name = name,
                .priority = priority,
                .directory = iterator->path(),
                .defaultSettingRoots = SettingRoots(*defaults),
                .filteredWeatherRules = ReadFilteredWeatherRules(iterator->path()),
                .filteredLightingTemplateRules = ReadFilteredLightingTemplateRules(iterator->path()),
                .interiorSliderSettings = std::move(interiorSliderLinks.settings),
                .ignoredInteriorSliderLinks = std::move(interiorSliderLinks.ignoredLinks),
            });
        }

        SortProfiles();
        discoveryInitialized = true;
        DetailedLogging::Info(
            "[TuningUtil] profiles={}",
            profiles.size());
        return profiles;
    }

    bool IsProfilePluginFiltered(const std::filesystem::path& a_directory)
    {
        return std::ranges::contains(pluginFilteredProfileDirectories, a_directory);
    }

    std::vector<std::string> GetProfilesWithSettings(
        const std::span<const std::string_view> a_settingRoots)
    {
        std::vector<std::string> result;
        for (const auto& profile : GetProfiles())
        {
            if (OwnsAnySetting(profile, a_settingRoots))
            {
                result.push_back(profile.name);
            }
        }
        return result;
    }

    std::optional<std::string> GetOverridingProfile(
        const std::string& a_profileName,
        const std::span<const std::string> a_settingPaths)
    {
        const auto* currentProfile = FindProfile(a_profileName);
        auto currentName = a_profileName;
        if (!currentProfile || a_settingPaths.empty() || !GetSettings(currentName).EnableProfile)
        {
            return std::nullopt;
        }

        std::vector<std::string> requestedPaths;
        for (const auto& path : a_settingPaths)
        {
            if (!IsProfileLocalSettingPath(path))
            {
                AddExpandedSettingPaths(requestedPaths, path);
            }
        }
        if (requestedPaths.empty())
        {
            return std::nullopt;
        }

        RefreshSettingOwnershipCache();
        struct Owner
        {
            std::string profile;
            std::size_t rank = 0;
        };
        std::unordered_map<std::string, std::vector<Owner>> owners;
        std::size_t rank = 0;
        for (std::size_t layer = 0; layer < 3; ++layer)
        {
            for (const auto& profile : profiles)
            {
                auto profileName = profile.name;
                const auto cache = settingOwnershipCache.find(Lowercase(profile.name));
                if (!GetSettings(profileName).EnableProfile || cache == settingOwnershipCache.end())
                {
                    ++rank;
                    continue;
                }
                for (const auto& ownedPath : cache->second.paths[layer])
                {
                    if (std::ranges::any_of(requestedPaths, [&](const auto& requestedPath)
                            { return SettingPathsOverlap(ownedPath, requestedPath); }))
                    {
                        owners[ownedPath].push_back({ profile.name, rank });
                    }
                }
                ++rank;
            }
        }

        std::optional<Owner> overriding;
        for (const auto& [path, pathOwners] : owners)
        {
            std::optional<std::size_t> currentRank;
            for (const auto& owner : pathOwners)
            {
                if (Config::IEquals(owner.profile, currentProfile->name) &&
                    (!currentRank || owner.rank > *currentRank))
                {
                    currentRank = owner.rank;
                }
            }
            for (const auto& owner : pathOwners)
            {
                if (!Config::IEquals(owner.profile, currentProfile->name) &&
                    (!currentRank || owner.rank > *currentRank) &&
                    (!overriding || owner.rank > overriding->rank) &&
                    ProfilesShareSettingTargets(currentProfile->name, owner.profile, path))
                {
                    overriding = owner;
                }
            }
        }
        return overriding ? std::optional<std::string>{ overriding->profile } : std::nullopt;
    }

    int GetProfilePriority(const std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->priority : 0;
    }

    std::filesystem::path ProfileDirectory(const std::string& a_profileName)
    {
        if (const auto* profile = FindProfile(a_profileName))
        {
            return profile->directory;
        }
        return kProfileRoot / ProfileName(a_profileName);
    }

    const std::vector<FilteredWeatherRule>& GetFilteredWeatherRules(const std::string& a_profileName)
    {
        static const std::vector<FilteredWeatherRule> empty;
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->filteredWeatherRules : empty;
    }

    const FilteredWeatherRule* FindFilteredWeatherRule(
        const std::string& a_profileName,
        const std::string_view a_id)
    {
        const auto& rules = GetFilteredWeatherRules(a_profileName);
        const auto found = std::ranges::find_if(rules, [&](const FilteredWeatherRule& a_rule)
            { return Config::IEquals(a_rule.id, a_id); });
        return found != rules.end() ? std::addressof(*found) : nullptr;
    }

    const std::vector<FilteredLightingTemplateRule>& GetFilteredLightingTemplateRules(
        const std::string& a_profileName)
    {
        static const std::vector<FilteredLightingTemplateRule> empty;
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->filteredLightingTemplateRules : empty;
    }

    const FilteredLightingTemplateRule* FindFilteredLightingTemplateRule(
        const std::string& a_profileName,
        const std::string_view a_id)
    {
        const auto& rules = GetFilteredLightingTemplateRules(a_profileName);
        const auto found = std::ranges::find_if(rules, [&](const FilteredLightingTemplateRule& a_rule)
            { return Config::IEquals(a_rule.id, a_id); });
        return found != rules.end() ? std::addressof(*found) : nullptr;
    }

    bool IgnoresInteriorSliderLink(
        const std::span<const std::string> a_profileNames,
        const std::string_view a_settingPath)
    {
        RefreshSettingOwnershipCache();
        const Profile* owner = nullptr;
        const auto consider = [&](const Profile& a_profile)
        {
            owner = std::addressof(a_profile);
        };
        const auto declaresSetting = [&](const Profile& a_profile)
        {
            return std::ranges::any_of(a_profile.interiorSliderSettings, [&](const auto& a_declared)
                { return SettingPathsOverlap(a_declared, a_settingPath); });
        };
        const auto ownsInLayer = [&](const Profile& a_profile, const std::size_t a_layer)
        {
            const auto cached = settingOwnershipCache.find(Lowercase(a_profile.name));
            return cached != settingOwnershipCache.end() &&
                   std::ranges::any_of(cached->second.paths[a_layer], [&](const auto& a_owned)
                       { return SettingPathsOverlap(a_owned, a_settingPath); });
        };

        for (std::size_t layer = 0; layer < 3; ++layer)
        {
            for (const auto& profileName : a_profileNames)
            {
                const auto* profile = FindProfile(profileName);
                if (!profile) continue;
                if (ownsInLayer(*profile, layer) || (layer == 0 && declaresSetting(*profile)))
                    consider(*profile);
            }
        }
        return owner && std::ranges::any_of(owner->ignoredInteriorSliderLinks, [&](const auto& a_ignored)
            { return SettingPathsOverlap(a_ignored, a_settingPath); });
    }

    bool ReloadFilteredRules()
    {
        auto changed = false;
        for (auto& profile : profiles)
        {
            auto weatherRules = ReadFilteredWeatherRules(profile.directory);
            auto lightingRules = ReadFilteredLightingTemplateRules(profile.directory);
            auto interiorSliderLinks = ReadInteriorSliderLinkRules(profile.directory);
            if (weatherRules == profile.filteredWeatherRules &&
                lightingRules == profile.filteredLightingTemplateRules &&
                interiorSliderLinks.settings == profile.interiorSliderSettings &&
                interiorSliderLinks.ignoredLinks == profile.ignoredInteriorSliderLinks)
            {
                continue;
            }
            std::optional<Settings> runtimeSettings;
            std::optional<std::string> presetPreviewUserLayer;
            if (const auto cached = settingsCache.find(Lowercase(profile.name)); cached != settingsCache.end())
            {
                runtimeSettings = cached->second.settings;
                presetPreviewUserLayer = cached->second.presetPreviewUserLayer;
            }
            profile.filteredWeatherRules = std::move(weatherRules);
            profile.filteredLightingTemplateRules = std::move(lightingRules);
            profile.interiorSliderSettings = std::move(interiorSliderLinks.settings);
            profile.ignoredInteriorSliderLinks = std::move(interiorSliderLinks.ignoredLinks);
            settingsCache.erase(Lowercase(profile.name));
            auto name = profile.name;
            auto& reloaded = GetSettings(name);
            if (runtimeSettings)
            {
                auto filteredDefaults = reloaded.filteredWeatherAdjustments;
                for (auto& [id, value] : filteredDefaults)
                {
                    const auto existing = std::ranges::find_if(
                        runtimeSettings->filteredWeatherAdjustments,
                        [&](const auto& a_entry) { return Config::IEquals(a_entry.first, id); });
                    if (existing != runtimeSettings->filteredWeatherAdjustments.end()) value = existing->second;
                }
                runtimeSettings->filteredWeatherAdjustments = std::move(filteredDefaults);
                auto filteredLightingDefaults = reloaded.filteredLightingTemplateAdjustments;
                for (auto& [id, value] : filteredLightingDefaults)
                {
                    const auto existing = std::ranges::find_if(
                        runtimeSettings->filteredLightingTemplateAdjustments,
                        [&](const auto& a_entry) { return Config::IEquals(a_entry.first, id); });
                    if (existing != runtimeSettings->filteredLightingTemplateAdjustments.end()) value = existing->second;
                }
                runtimeSettings->filteredLightingTemplateAdjustments = std::move(filteredLightingDefaults);
                reloaded = std::move(*runtimeSettings);
                settingsCache.at(Lowercase(profile.name)).presetPreviewUserLayer = std::move(presetPreviewUserLayer);
                ++settingsRevision;
            }
            changed = true;
            DetailedLogging::Info(
                "[TuningUtil] {} filters | weather={} | lightingTemplates={}",
                profile.name,
                profile.filteredWeatherRules.size(),
                profile.filteredLightingTemplateRules.size());
        }
        return changed;
    }

    Settings& GetSettings(std::string& a_profileName)
    {
        static Settings rejected{};
        const auto name = ProfileName(a_profileName);
        const auto* profile = FindProfile(name);
        if (name.empty() || !profile)
        {
            logger::warn("[TuningUtil] settings access rejected | profile={} unknown", a_profileName);
            return rejected;
        }
        const auto cacheKey = Lowercase(name);
        if (const auto cached = settingsCache.find(cacheKey); cached != settingsCache.end())
        {
            return cached->second.settings;
        }

        auto settings = LoadStoredSettings(*profile);
        if (!settings)
        {
            logger::warn("[TuningUtil] {} defaults unreadable | fallback=neutral", name);
            return settingsCache.try_emplace(cacheKey, CachedSettings{}).first->second.settings;
        }
        return settingsCache.try_emplace(cacheKey, std::move(*settings)).first->second.settings;
    }

    Settings ResolveSettingsStack(const std::span<const std::string> a_profileNames)
    {
        std::string error;
        auto combined = GlobalDefaultsText();
        for (const auto& profileName : a_profileNames)
        {
            const auto* profile = FindProfile(profileName);
            const auto defaults = profile ? ReadText(ProfileDefaultsPath(*profile)) : std::nullopt;
            if (!defaults)
            {
                continue;
            }
            if (auto overlaid = JsonOverlay::Overlay(combined, *defaults, error))
            {
                combined = std::move(*overlaid);
            }
            else
            {
                logger::warn("[TuningUtil] {} stack defaults failed | {}", profileName, error);
            }
        }
        for (const auto& profileName : a_profileNames)
        {
            const auto* profile = FindProfile(profileName);
            const auto activePresets = profile ? ActivePresetSettingsText(*profile, error) : std::nullopt;
            if (!activePresets)
            {
                continue;
            }
            if (auto overlaid = JsonOverlay::Overlay(combined, *activePresets, error))
            {
                combined = std::move(*overlaid);
            }
            else
            {
                logger::warn("[TuningUtil] {} stack presets failed | {}", profileName, error);
            }
        }
        for (const auto& profileName : a_profileNames)
        {
            const auto* profile = FindProfile(profileName);
            const auto user = profile ? CurrentUserLayer(*profile, error) : std::nullopt;
            if (!user)
            {
                continue;
            }
            if (auto overlaid = JsonOverlay::Overlay(combined, *user, error))
            {
                combined = std::move(*overlaid);
            }
            else
            {
                logger::warn("[TuningUtil] {} stack user settings failed | {}", profileName, error);
            }
        }
        return ParseSettings(combined, "resolved profile stack").value_or(Settings{});
    }

    std::optional<std::string> SerializePresetSettings(
        std::string& a_profileName,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto defaultText = profile ? LocalDefaultsText(*profile, a_error) : std::nullopt;
        if (!profile || !defaultText || !ParseSettings(*defaultText, ProfileDefaultsPath(*profile)))
        {
            a_error = "The profile defaults could not be read.";
            return std::nullopt;
        }

        const auto current = SerializeSettings(GetSettings(a_profileName));
        const auto declared = CombineDeclaredSettings(current, *defaultText, a_error);
        if (!declared)
        {
            a_error = "The profile settings could not be prepared for a preset.";
            return std::nullopt;
        }
        return CompactLinkArrays(*declared);
    }

    bool ApplyPresetPreview(
        std::string& a_profileName,
        const std::string_view a_effectivePresetSettings,
        const std::string_view a_changedPresetSettings,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        if (!profile)
        {
            a_error = "The profile is unavailable.";
            return false;
        }

        (void)GetSettings(a_profileName);
        const auto cached = settingsCache.find(Lowercase(profile->name));
        if (cached == settingsCache.end())
        {
            a_error = "The profile settings cache is unavailable.";
            return false;
        }

        auto userLayer = cached->second.presetPreviewUserLayer;
        if (!userLayer)
        {
            userLayer = CurrentUserLayer(*profile, a_error);
        }
        const auto effective = JsonOverlay::ProjectLike(a_effectivePresetSettings, cached->second.localDefaults, a_error);
        const auto changed = effective ? JsonOverlay::ProjectLike(a_changedPresetSettings, cached->second.localDefaults, a_error) : std::nullopt;
        const auto presetDefaults = changed ? JsonOverlay::Overlay(cached->second.localDefaults, *effective, a_error) : std::nullopt;
        const auto withUser = presetDefaults && userLayer ? JsonOverlay::Overlay(*presetDefaults, *userLayer, a_error) : std::nullopt;
        const auto preview = withUser ? JsonOverlay::Overlay(*withUser, *changed, a_error) : std::nullopt;
        auto settings = preview ? ParseSettings(*preview, ProfileDefaultsPath(*profile)) : std::nullopt;
        if (!settings)
        {
            if (a_error.empty()) a_error = "The preset preview settings could not be composed.";
            return false;
        }

        cached->second.settings = std::move(*settings);
        cached->second.presetPreviewUserLayer = std::move(userLayer);
        ApplySettings();
        return true;
    }

    bool ApplyPresetAndRemoveUserOverrides(
        std::string& a_profileName,
        const std::string_view a_presetSettings,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto schema = profile ? LocalDefaultsText(*profile, a_error) : std::nullopt;
        const auto declared = schema ? JsonOverlay::ProjectLike(a_presetSettings, *schema, a_error) : std::nullopt;
        if (!profile || !declared)
        {
            if (a_error.empty()) a_error = "The selected preset settings could not be read.";
            return false;
        }

        const auto userPath = UserSettingsPath(*profile);
        if (std::filesystem::is_regular_file(userPath))
        {
            const auto user = ReadText(userPath);
            const auto retained = user ? JsonOverlay::RemoveLike(*user, *declared, a_error) : std::nullopt;
            if (!retained || !WriteUserSettings(*profile, *retained))
            {
                if (a_error.empty()) a_error = "Matching user settings could not be removed.";
                return false;
            }
            DetailedLogging::Info("[TuningUtil] {} user overrides | matchingPreset removed", profile->name);
        }
        return RestoreSettings(a_profileName);
    }

    bool SaveSettings(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        return profile && WriteSettings(*profile, a_profileName, GetSettings(a_profileName));
    }

    bool PromoteUserSettingsToProfile(
        std::string& a_profileName,
        std::string& a_error)
    {
        a_error.clear();
        (void)GetProfiles();
        const auto profile = std::ranges::find_if(profiles, [&](const Profile& a_candidate)
            { return Config::IEquals(a_candidate.name, a_profileName); });
        if (profile == profiles.end())
        {
            a_error = "The profile is unavailable.";
            return false;
        }

        const auto canonicalName = profile->name;
        auto resolvedName = canonicalName;
        (void)GetSettings(resolvedName);
        const auto cached = settingsCache.find(Lowercase(profile->name));
        if (cached == settingsCache.end())
        {
            a_error = "The profile settings cache is unavailable.";
            return false;
        }

        auto userSettings = cached->second.presetPreviewUserLayer;
        if (!userSettings)
        {
            userSettings = CurrentUserLayer(*profile, a_error);
        }
        const auto userDocument = userSettings ? Parse(*userSettings) : nullptr;
        auto* userRoot = userDocument ? yyjson_doc_get_root(userDocument.get()) : nullptr;
        if (!yyjson_is_obj(userRoot))
        {
            if (a_error.empty()) a_error = "The user settings could not be read.";
            return false;
        }
        if (yyjson_obj_size(userRoot) == 0)
        {
            a_error = "There are no user settings to make permanent.";
            return false;
        }

        const auto defaultsPath = ProfileDefaultsPath(*profile);
        const auto profileSettings = ReadText(defaultsPath);
        const auto merged = profileSettings ?
                                JsonOverlay::Overlay(*profileSettings, *userSettings, a_error) :
                                std::nullopt;
        if (!merged || !ParseSettings(*merged, defaultsPath))
        {
            if (a_error.empty()) a_error = "The profile settings could not be prepared.";
            return false;
        }
        if (!WriteTextAtomically(defaultsPath, CompactLinkArrays(*merged), a_error))
        {
            return false;
        }

        const auto userPath = UserSettingsPath(*profile);
        std::error_code removeError;
        auto userSettingsCleared = true;
        const auto hasUserSettingsFile = std::filesystem::is_regular_file(userPath, removeError);
        if (removeError)
        {
            userSettingsCleared = false;
            a_error = std::format(
                "The profile settings were updated, but {} could not be inspected: {}",
                userPath.string(),
                removeError.message());
            logger::warn("[TuningUtil] permanent settings warning | {}", a_error);
        }
        else if (hasUserSettingsFile &&
                 !std::filesystem::remove(userPath, removeError) &&
                 !WriteUserSettings(*profile, "{}"))
        {
            userSettingsCleared = false;
            a_error = std::format(
                "The profile settings were updated, but {} could not be cleared: {}",
                userPath.string(),
                removeError ? removeError.message() : "unknown error");
            logger::warn("[TuningUtil] permanent settings warning | {}", a_error);
        }

        profile->defaultSettingRoots = SettingRoots(*merged);
        settingsCache.erase(Lowercase(canonicalName));
        WeatherPatcher::InvalidatePresetCache();
        (void)GetSettings(resolvedName);
        ApplySettings();
        if (userSettingsCleared)
            logger::info(
                "[TuningUtil] {} user settings | permanent=true | path={}",
                canonicalName,
                defaultsPath.string());
        a_profileName = canonicalName;
        return userSettingsCleared;
    }

    bool SavePageSettings(std::string& a_profileName, const std::vector<std::string>& a_scopes)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaultText = profile ? PresetDefaultsText(*profile, error) : std::nullopt;
        if (!profile || !defaultText || !ParseSettings(*defaultText, ProfileDefaultsPath(*profile)))
        {
            return false;
        }

        const auto paths = ExpandSettingScopes(a_scopes);
        const auto current = SerializeSettings(GetSettings(a_profileName));
        const auto allDifferences = JsonOverlay::Difference(current, *defaultText, error);
        const auto pageDifferences = allDifferences ? JsonOverlay::ProjectPaths(*allDifferences, paths, error) : std::nullopt;
        const auto existing = ReadText(UserSettingsPath(*profile)).value_or("{}");
        const auto retainedExisting = JsonOverlay::RemovePaths(existing, paths, error);
        const auto difference = pageDifferences && retainedExisting ?
                                    JsonOverlay::Overlay(*retainedExisting, *pageDifferences, error) :
                                    std::nullopt;
        if (!difference || !WriteUserSettings(*profile, *difference))
        {
            logger::warn("[TuningUtil] {} page save failed | {}", a_profileName, error);
            return false;
        }
        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            cached->second.explicitUserSettings = *difference;
            if (cached->second.presetPreviewUserLayer)
            {
                const auto currentUser = CurrentUserLayer(*profile, error);
                if (currentUser) cached->second.presetPreviewUserLayer = *currentUser;
            }
        }
        return true;
    }

    bool RestoreSettings(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        if (!profile)
        {
            return false;
        }
        settingsCache.erase(Lowercase(profile->name));
        (void)GetSettings(a_profileName);
        ApplySettings();
        return true;
    }

    bool RestorePageSettings(std::string& a_profileName, const std::vector<std::string>& a_scopes)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaultText = profile ? PresetDefaultsText(*profile, error) : std::nullopt;
        const auto stored = profile && defaultText ? StoredSettingsText(*profile, *defaultText, error) : std::nullopt;
        const auto paths = ExpandSettingScopes(a_scopes);
        const auto page = stored ? JsonOverlay::ProjectPaths(*stored, paths, error) : std::nullopt;
        if (!profile || !page)
        {
            return false;
        }
        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            const auto retained = JsonOverlay::RemovePaths(cached->second.explicitUserSettings, paths, error);
            const auto user = ReadText(UserSettingsPath(*profile)).value_or("{}");
            const auto restored = JsonOverlay::ProjectPaths(user, paths, error);
            const auto explicitSettings = retained && restored ? JsonOverlay::Overlay(*retained, *restored, error) : std::nullopt;
            if (explicitSettings) cached->second.explicitUserSettings = std::move(*explicitSettings);
        }
        return ApplySettingsPatch(*profile, a_profileName, *page);
    }

    bool ResetAllSettingsToDefault(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaults = profile ? LocalDefaultsText(*profile, error) : std::nullopt;
        if (!profile || !defaults)
        {
            return false;
        }
        auto parsed = ParseSettings(*defaults, ProfileDefaultsPath(*profile));
        if (!parsed)
        {
            return false;
        }
        auto& cached = settingsCache[Lowercase(profile->name)];
        cached.settings = std::move(*parsed);
        cached.localDefaults = *defaults;
        const auto activePresets = ActivePresetSettingsText(*profile, error).value_or("{}");
        cached.presetDefaults = JsonOverlay::Overlay(*defaults, activePresets, error).value_or(*defaults);
        cached.explicitUserSettings = "{}";
        cached.presetPreviewUserLayer.reset();
        ApplySettings();
        return true;
    }

    bool ResetSettingsToDefault(std::string& a_profileName, const std::vector<std::string>& a_scopes)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaults = profile ? LocalDefaultsText(*profile, error) : std::nullopt;
        const auto paths = ExpandSettingScopes(a_scopes);
        const auto page = defaults ? JsonOverlay::ProjectPaths(*defaults, paths, error) : std::nullopt;
        if (!profile || !page)
        {
            return false;
        }
        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            if (auto retained = JsonOverlay::RemovePaths(cached->second.explicitUserSettings, paths, error))
            {
                cached->second.explicitUserSettings = std::move(*retained);
            }
        }
        return ApplySettingsPatch(*profile, a_profileName, *page);
    }

    void ApplySettings(const bool a_commitLightPlacer)
    {
        if (runtimeStateReleased)
        {
            return;
        }
        ++settingsRevision;
        SynchronizeProfilePriorities();
        WeatherPatcher::ApplyAllSettings();
        LightingPatcher::ApplyAllSettings(a_commitLightPlacer);
        ImageSpacePatcher::ApplyFilmicCurveWhitePoint();
    }

    std::uint64_t GetSettingsRevision()
    {
        return settingsRevision;
    }

    void ApplyDataLoaded()
    {
        ++settingsRevision;
        runtimeStateReleased = false;
        startupSettingTargetOverlapCache.clear();
        startupSettingTargetOverlapsCaptured = false;
        pluginDependencyFilterReady = true;
        InvalidateDiscoveryCaches();
        (void)GetProfiles();
        for (auto& profile : profiles)
        {
            auto name = profile.name;
            (void)GetSettings(name);
        }
        SynchronizeProfilePriorities();
        WeatherPatcher::ApplyDataLoaded();
        LightingPatcher::ApplyDataLoaded();
        if (TuningSettings::IsTuningMenuEnabledForSession())
        {
            CaptureStartupSettingTargetOverlaps();
        }
        CSTonemapping::Initialize();
        ImageSpacePatcher::ApplyFilmicCurveWhitePoint();
        if (!TuningSettings::IsTuningMenuEnabledForSession())
        {
            WeatherPatcher::ReleaseRuntimeState();
            LightingPatcher::ReleaseRuntimeState();
            ImageSpacePatcher::ReleaseRuntimeState();
            CSTonemapping::ReleaseRuntimeState();
            profiles = {};
            settingsCache = {};
            globalDefaultsCache.reset();
            discoveryInitialized = false;
            runtimeStateReleased = true;
            logger::info("[TuningUtil] startupOnly=true | pointLightState=retained");
        }
    }
}  // namespace MPL::TuningUtil
