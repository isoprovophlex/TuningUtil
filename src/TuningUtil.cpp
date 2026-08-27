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
        bool runtimeStateReleased = false;
        std::uint64_t settingsRevision = 0;
        std::vector<Profile> profiles;
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
                        result.push_back({ std::move(path), 1.0, true });
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
                    result.push_back({ std::move(*path), scale, ignoreLink });
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
                    logger::warn("Ignored filtered slider {} in {} because time {} is unsupported", a_id, a_source.string(), configured);
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
                logger::warn("Ignored filtered slider in {} because it has no stable id", a_source.string());
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
                    logger::warn("Ignored filtered slider {} in {} because setting {} is unsupported", rule.id, a_source.string(), specification.path);
                    return std::nullopt;
                }
                if (!rule.settings.empty() && setting->operation != rule.settings.front().operation)
                {
                    logger::warn("Ignored filtered slider {} in {} because grouped settings use different operations", rule.id, a_source.string());
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
                    logger::warn("Ignored filtered slider {} in {} because local link {} is unsupported",
                        rule.id, a_source.string(), *rule.localLink);
                    return std::nullopt;
                }
            }
            if (rule.hueScales && rule.settings.front().operation != FilteredWeatherOperation::saturation)
            {
                logger::warn("Ignored filtered slider {} in {} because unique hue scales require saturation settings",
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
            const auto kind = Lowercase(Trim(JsonString(a_control, "kind").value_or("")));
            const auto times = JsonStrings(a_control, "times");
            const auto filtered = !times.empty() || yyjson_is_obj(yyjson_obj_get(a_control, "weatherFilter")) ||
                                  yyjson_is_str(yyjson_obj_get(a_control, "localLink")) ||
                                  yyjson_is_obj(yyjson_obj_get(a_control, "hueScales")) ||
                                  HasStructuredSliderSettings(a_control);
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
                logger::warn("Ignored filtered settings editor module in {} because it has no stable id", a_source.string());
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
                            logger::warn("Ignored conflicting duplicate filtered slider id {} in {}", rule.id, path.string());
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

        std::string FilteredWeatherDefaultsText(const Profile& a_profile)
        {
            struct Defaults
            {
                std::map<std::string, double> filteredWeatherAdjustments;
            } defaults;
            for (const auto& rule : a_profile.filteredWeatherRules)
            {
                defaults.filteredWeatherAdjustments.emplace(rule.id, rule.defaultValue);
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
                R"({"weatherInclusions":{"formIDs":[],"contains":[]},"weatherExclusions":{"formIDs":[],"contains":[]},"pluginInclusions":{"exact":[],"contains":[]},"pluginExclusions":{"exact":[],"contains":[]},"effectLightingInclusions":{"formIDs":[],"contains":[]},"effectLightingExclusions":{"formIDs":[],"contains":[]},"effectLightingPluginInclusions":{"exact":[],"contains":[]},"effectLightingPluginExclusions":{"exact":[],"contains":[]},"lightingTemplateInclusions":{"formIDs":[],"contains":[]},"lightingTemplateExclusions":{"formIDs":[],"contains":[]},"lightingTemplatePluginInclusions":{"exact":[],"contains":[]},"lightingTemplatePluginExclusions":{"exact":[],"contains":[]}})";
            std::string normalizationError;
            const auto normalized = JsonOverlay::Overlay(filterSchema, a_json, normalizationError);
            if (!normalized)
            {
                logger::warn("Could not normalize TuningUtil settings {}: {}", a_source.string(), normalizationError);
                return std::nullopt;
            }
            const auto parsed = rfl::json::read<Settings, rfl::DefaultIfMissing>(*normalized);
            if (!parsed)
            {
                logger::warn("Could not load TuningUtil settings {}: {}", a_source.string(), parsed.error().what());
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
                "TuningUtil global defaults {} are missing or invalid; using compiled emergency defaults",
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
                       JsonOverlay::Overlay(*local, FilteredWeatherDefaultsText(a_profile), a_error) :
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
                logger::warn("Could not compose TuningUtil defaults for {}: {}", a_profile.name, error);
                return std::nullopt;
            }
            const auto activePresetSettings = ActivePresetSettingsText(a_profile, error);
            const auto presetDefaults = activePresetSettings ?
                                            JsonOverlay::Overlay(*localDefaults, *activePresetSettings, error) :
                                            std::nullopt;
            auto defaults = presetDefaults ? ParseSettings(*presetDefaults, ProfileDefaultsPath(a_profile)) : std::nullopt;
            if (!defaults)
            {
                logger::warn("Could not compose active preset settings for {}: {}", a_profile.name, error);
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
                    DetailedLogging::Info("Applied sparse TuningUtil user overrides {}", UserSettingsPath(a_profile).string());
                }
            }
            else if (!stored)
            {
                logger::warn("Could not merge TuningUtil user settings {}: {}", UserSettingsPath(a_profile).string(), error);
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
                logger::warn("Could not apply TuningUtil settings patch for {}: {}", a_profileName, error);
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
                logger::warn("Could not create TuningUtil user directory {}: {}", path.parent_path().string(), fileError.message());
                return false;
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << CompactLinkArrays(std::string(a_settings)) << '\n';
            if (!file)
            {
                logger::warn("Could not save TuningUtil user settings {}", path.string());
                return false;
            }
            logger::info("Saved sparse TuningUtil user overrides to {}", path.string());
            return true;
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
                logger::warn("Refused to save TuningUtil profile {} because its defaults are unreadable", a_profileName);
                return false;
            }
            const auto current = SerializeSettings(a_settings);
            const auto declared = CombineDeclaredSettings(current, *defaultText, error);
            if (!declared)
            {
                logger::warn("Could not retain declared settings while saving {}: {}", a_profileName, error);
                return false;
            }
            const auto difference = JsonOverlay::Difference(*declared, *defaultText, error);
            if (!difference)
            {
                logger::warn("Could not create sparse TuningUtil settings for {}: {}", a_profileName, error);
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
        settingsCache.clear();
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
                    "Ignored TuningUtil profile {} because profileSettings.json has no valid profile name",
                    iterator->path().string());
                continue;
            }
            if (!ParseSettings(*defaults, defaultPath))
            {
                logger::warn("Ignored TuningUtil profile {} because its defaults are invalid", name);
                continue;
            }
            if (std::ranges::any_of(profiles, [&](const Profile& a_profile)
                    { return Config::IEquals(a_profile.name, name); }))
            {
                logger::warn("Ignored duplicate TuningUtil profile {} in {}", name, iterator->path().string());
                continue;
            }
            const auto user = ReadText(kUserRoot / iterator->path().filename() / "userSettings.json");
            auto priority = IntegerMember(GlobalDefaultsText(), "profilePriority").value_or(0);
            priority = IntegerMember(*defaults, "profilePriority").value_or(priority);
            if (user)
            {
                priority = IntegerMember(*user, "profilePriority").value_or(priority);
            }
            profiles.push_back({
                name,
                priority,
                iterator->path(),
                SettingRoots(*defaults),
                ReadFilteredWeatherRules(iterator->path()) });
        }

        SortProfiles();
        discoveryInitialized = true;
        DetailedLogging::Info(
            "Cached {} settings-defined TuningUtil profile(s); SKSE menu definitions are optional",
            profiles.size());
        return profiles;
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

    bool ReloadFilteredWeatherRules()
    {
        auto changed = false;
        for (auto& profile : profiles)
        {
            auto rules = ReadFilteredWeatherRules(profile.directory);
            if (rules == profile.filteredWeatherRules)
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
            profile.filteredWeatherRules = std::move(rules);
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
                reloaded = std::move(*runtimeSettings);
                settingsCache.at(Lowercase(profile.name)).presetPreviewUserLayer = std::move(presetPreviewUserLayer);
                ++settingsRevision;
            }
            changed = true;
            DetailedLogging::Info(
                "Reloaded {} filtered weather rule(s) for TuningUtil profile {}",
                profile.filteredWeatherRules.size(),
                profile.name);
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
            logger::warn("Rejected TuningUtil settings access for unknown profile {}", a_profileName);
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
            logger::warn("TuningUtil profile {} has no readable defaults; using neutral settings", name);
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
                logger::warn("Could not compose defaults for TuningUtil profile {}: {}", profileName, error);
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
                logger::warn("Could not compose active presets for TuningUtil profile {}: {}", profileName, error);
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
                logger::warn("Could not compose user settings for TuningUtil profile {}: {}", profileName, error);
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
            DetailedLogging::Info("Removed user overrides matching the selected preset for {}", profile->name);
        }
        return RestoreSettings(a_profileName);
    }

    bool SaveSettings(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        return profile && WriteSettings(*profile, a_profileName, GetSettings(a_profileName));
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
            logger::warn("Could not save TuningUtil page settings for {}: {}", a_profileName, error);
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
        if (!discoveryInitialized)
        {
            InvalidateDiscoveryCaches();
        }
        (void)GetProfiles();
        for (auto& profile : profiles)
        {
            auto name = profile.name;
            (void)GetSettings(name);
        }
        SynchronizeProfilePriorities();
        WeatherPatcher::ApplyDataLoaded();
        LightingPatcher::ApplyDataLoaded();
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
            logger::info(
                "TuningUtil applied profiles in startup-only mode and retained point-light state for runtime reference corrections");
        }
    }
}  // namespace MPL::TuningUtil
