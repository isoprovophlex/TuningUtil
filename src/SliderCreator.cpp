#include <SliderCreator.h>
#include <SliderSettingCatalog.h>
#include <PresetCatalog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <format>
#include <memory>
#include <ranges>
#include <system_error>
#include <Windows.h>
#include <yyjson.h>

namespace MPL::SliderCreator
{
    namespace
    {
        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const { yyjson_doc_free(a_document); }
        };

        struct MutableDocumentDeleter
        {
            void operator()(yyjson_mut_doc* a_document) const { yyjson_mut_doc_free(a_document); }
        };

        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;
        using MutableDocument = std::unique_ptr<yyjson_mut_doc, MutableDocumentDeleter>;

        std::string Trim(std::string a_value)
        {
            const auto first = a_value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = a_value.find_last_not_of(" \t\r\n");
            return a_value.substr(first, last - first + 1);
        }

        bool IEquals(const std::string_view a_left, const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                   std::ranges::equal(a_left, a_right, [](const unsigned char a_lhs, const unsigned char a_rhs)
                   {
                       return std::tolower(a_lhs) == std::tolower(a_rhs);
                   });
        }

        std::optional<std::string> StringMember(yyjson_val* a_object, const std::string_view a_key)
        {
            auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
            return yyjson_is_str(value) ?
                       std::optional<std::string>{ std::string(yyjson_get_str(value), yyjson_get_len(value)) } :
                       std::nullopt;
        }

        std::optional<double> NumberMember(yyjson_val* a_object, const std::string_view a_key)
        {
            auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
            if (!yyjson_is_num(value)) return std::nullopt;
            const auto number = yyjson_get_num(value);
            return std::isfinite(number) ? std::optional<double>{ number } : std::nullopt;
        }

        bool BooleanMember(yyjson_val* a_object, const std::string_view a_key, const bool a_fallback = false)
        {
            auto* value = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
            return yyjson_is_bool(value) ? yyjson_get_bool(value) : a_fallback;
        }

        std::vector<std::string> StringArray(yyjson_val* a_object, const std::string_view a_key)
        {
            std::vector<std::string> result;
            auto* values = yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
            if (!yyjson_is_arr(values)) return result;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* value = nullptr;
            yyjson_arr_foreach(values, index, maximum, value)
            {
                if (!yyjson_is_str(value)) continue;
                auto text = Trim(std::string(yyjson_get_str(value), yyjson_get_len(value)));
                if (!text.empty() && !std::ranges::any_of(result, [&](const auto& a_existing)
                        { return IEquals(a_existing, text); }))
                    result.push_back(std::move(text));
            }
            return result;
        }

        Filter ReadFilter(yyjson_val* a_object)
        {
            return {
                .formIDs = StringArray(a_object, "formIDs"),
                .contains = StringArray(a_object, "contains"),
                .locationTypes = StringArray(a_object, "locationTypes"),
                .multiLocationExceptions = StringArray(a_object, "multiLocationExceptions"),
            };
        }

        std::optional<HueScales> ReadHueScales(yyjson_val* a_control)
        {
            auto* value = yyjson_is_obj(a_control) ? yyjson_obj_get(a_control, "hueScales") : nullptr;
            if (!yyjson_is_obj(value)) return std::nullopt;
            return HueScales{
                .red = NumberMember(value, "red").value_or(1.0),
                .orange = NumberMember(value, "orange").value_or(1.0),
                .yellow = NumberMember(value, "yellow").value_or(1.0),
                .green = NumberMember(value, "green").value_or(1.0),
                .teal = NumberMember(value, "teal").value_or(1.0),
                .blue = NumberMember(value, "blue").value_or(1.0),
                .magenta = NumberMember(value, "magenta").value_or(1.0),
            };
        }

        std::optional<CustomLinks> ReadCustomLinks(yyjson_val* a_control)
        {
            auto* object = yyjson_is_obj(a_control) ? yyjson_obj_get(a_control, "customLinks") : nullptr;
            if (!yyjson_is_obj(object)) return std::nullopt;

            CustomLinks result;
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
                auto targetName = Trim(std::string(yyjson_get_str(key), yyjson_get_len(key)));
                auto sourceName = Trim(std::string(yyjson_get_str(source), yyjson_get_len(source)));
                const auto scaleValue = yyjson_is_num(scale) ? yyjson_get_real(scale) : 1.0;
                if (!targetName.empty() && !sourceName.empty() && std::isfinite(scaleValue))
                    result.insert_or_assign(
                        std::move(targetName),
                        std::tuple{ std::move(sourceName), scaleValue });
            }
            return result;
        }

        std::optional<Target> ReadTarget(yyjson_val* a_value, bool& a_structured)
        {
            if (yyjson_is_str(a_value))
            {
                auto setting = Trim(std::string(yyjson_get_str(a_value), yyjson_get_len(a_value)));
                return setting.empty() ? std::nullopt : std::optional<Target>{ { std::move(setting), 1.0 } };
            }
            if (!yyjson_is_obj(a_value)) return std::nullopt;
            a_structured = true;
            auto setting = StringMember(a_value, "setting");
            if (!setting || (setting = Trim(std::move(*setting)), setting->empty())) return std::nullopt;
            return Target{
                .setting = std::move(*setting),
                .scale = NumberMember(a_value, "scale").value_or(1.0),
            };
        }

        bool IsLightingLinkableSetting(const std::string_view a_setting)
        {
            return a_setting.starts_with("lightBrightnessMultiplier.") ||
                   a_setting.starts_with("lightSaturationMultiplier.") ||
                   a_setting.starts_with("lightHueShift.");
        }

        Definition ReadDefinition(yyjson_val* a_control)
        {
            Definition result;
            result.id = StringMember(a_control, "id").value_or("");
            result.label = StringMember(a_control, "label").value_or("");
            result.customLinks = ReadCustomLinks(a_control);
            result.hueScales = ReadHueScales(a_control);
            result.ignoreProfileFilters = BooleanMember(a_control, "ignoreProfileFilters");
            result.invert = BooleanMember(a_control, "invert");
            result.minimum = NumberMember(a_control, "min");
            result.maximum = NumberMember(a_control, "max");
            result.step = NumberMember(a_control, "step");
            result.width = NumberMember(a_control, "width");
            result.format = StringMember(a_control, "format").value_or("");

            auto structured = false;
            if (auto* settings = yyjson_obj_get(a_control, "settings"); yyjson_is_arr(settings))
            {
                std::size_t index = 0;
                std::size_t maximum = 0;
                yyjson_val* value = nullptr;
                yyjson_arr_foreach(settings, index, maximum, value)
                {
                    if (auto target = ReadTarget(value, structured)) result.settings.push_back(std::move(*target));
                }
            }
            if (result.settings.empty())
            {
                if (auto* setting = yyjson_obj_get(a_control, "setting"))
                    if (auto target = ReadTarget(setting, structured)) result.settings.push_back(std::move(*target));
            }

            if (auto* times = yyjson_obj_get(a_control, "times"); yyjson_is_arr(times))
            {
                result.useTimes = true;
                result.times.fill(false);
                for (const auto& time : StringArray(a_control, "times"))
                {
                    if (IEquals(time, "all")) result.times.fill(true);
                    else if (IEquals(time, "sunrise")) result.times[0] = true;
                    else if (IEquals(time, "day")) result.times[1] = true;
                    else if (IEquals(time, "sunset")) result.times[2] = true;
                    else if (IEquals(time, "night")) result.times[3] = true;
                    else if (IEquals(time, "duskAndDawn")) result.times[0] = result.times[2] = true;
                }
            }
            if (auto* weatherFilter = yyjson_obj_get(a_control, "weatherFilter"); yyjson_is_obj(weatherFilter))
            {
                result.include = ReadFilter(yyjson_obj_get(weatherFilter, "include"));
                result.exclude = ReadFilter(yyjson_obj_get(weatherFilter, "exclude"));
            }
            if (auto* lightingTemplateFilter = yyjson_obj_get(a_control, "lightingTemplateFilter");
                yyjson_is_obj(lightingTemplateFilter))
            {
                result.filterDomain = FilterDomain::lightingTemplate;
                result.include = ReadFilter(yyjson_obj_get(lightingTemplateFilter, "include"));
                result.exclude = ReadFilter(yyjson_obj_get(lightingTemplateFilter, "exclude"));
            }
            if (auto* baseLightFilter = yyjson_obj_get(a_control, "baseLightFilter");
                yyjson_is_obj(baseLightFilter))
            {
                result.filterDomain = FilterDomain::baseLight;
                result.include = ReadFilter(yyjson_obj_get(baseLightFilter, "include"));
                result.exclude = ReadFilter(yyjson_obj_get(baseLightFilter, "exclude"));
            }
            if (auto* baseObjectFilter = yyjson_obj_get(a_control, "baseObjectFilter");
                yyjson_is_obj(baseObjectFilter))
            {
                result.filterDomain = FilterDomain::baseObject;
                result.include = ReadFilter(yyjson_obj_get(baseObjectFilter, "include"));
                result.exclude = ReadFilter(yyjson_obj_get(baseObjectFilter, "exclude"));
            }
            result.filtered = structured || result.ignoreProfileFilters || result.useTimes ||
                              result.hueScales ||
                              result.filterDomain == FilterDomain::lightingTemplate ||
                              result.filterDomain == FilterDomain::baseLight ||
                              result.filterDomain == FilterDomain::baseObject ||
                              !result.include.formIDs.empty() || !result.include.contains.empty() ||
                              !result.exclude.formIDs.empty() || !result.exclude.contains.empty();
            return result;
        }

        bool AddString(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_object,
            const std::string_view a_key,
            const std::string_view a_value)
        {
            auto* key = yyjson_mut_strncpy(a_document, a_key.data(), a_key.size());
            auto* value = yyjson_mut_strncpy(a_document, a_value.data(), a_value.size());
            return key && value && yyjson_mut_obj_add(a_object, key, value);
        }

        bool ReplaceStringArray(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_object,
            const std::string_view a_key,
            const std::vector<std::string>& a_values)
        {
            auto* values = yyjson_mut_arr(a_document);
            if (!values) return false;
            for (const auto& configured : a_values)
            {
                const auto value = Trim(configured);
                if (value.empty()) continue;
                auto* stringValue = yyjson_mut_strncpy(a_document, value.data(), value.size());
                if (!stringValue || !yyjson_mut_arr_append(values, stringValue)) return false;
            }
            yyjson_mut_obj_remove_key(a_object, a_key.data());
            return yyjson_mut_obj_add_val(a_document, a_object, a_key.data(), values);
        }

        bool SetProfilePageLast(yyjson_mut_doc* a_document, yyjson_mut_val* a_root)
        {
            auto* profilePage = yyjson_mut_is_obj(a_root) ? yyjson_mut_obj_get(a_root, "profilePage") : nullptr;
            if (!yyjson_mut_is_obj(profilePage)) return true;
            auto* pages = yyjson_mut_obj_get(a_root, "pages");
            if (!yyjson_mut_is_arr(pages)) return false;
            yyjson_mut_obj_remove_key(profilePage, "order");
            auto* order = yyjson_mut_uint(a_document, yyjson_mut_arr_size(pages));
            return order && yyjson_mut_obj_add_val(a_document, profilePage, "order", order);
        }

        bool ModuleHasDisplayName(const std::string_view a_kind)
        {
            static constexpr std::array kinds{
                std::string_view("slider"),
                std::string_view("text"),
                std::string_view("description"),
                std::string_view("separatorText"),
                std::string_view("dropBoxStart"),
                std::string_view("ambientWithinGauge"),
                std::string_view("ambientBetweenGauge"),
                std::string_view("sunlightWithinGauge"),
                std::string_view("sunlightBetweenGauge"),
                std::string_view("links"),
            };
            return std::ranges::any_of(kinds, [&](const auto kind) { return IEquals(a_kind, kind); });
        }

        bool RenameMutableModule(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_module,
            const std::string& a_name,
            std::string& a_error)
        {
            const auto name = Trim(a_name);
            auto* kindValue = yyjson_mut_is_obj(a_module) ? yyjson_mut_obj_get(a_module, "type") : nullptr;
            if (!yyjson_mut_is_str(kindValue))
            {
                a_error = "The selected module is unavailable.";
                return false;
            }
            const std::string_view kind(yyjson_mut_get_str(kindValue), yyjson_mut_get_len(kindValue));
            if (!ModuleHasDisplayName(kind))
            {
                a_error = "This module does not have a display name.";
                return false;
            }

            yyjson_mut_obj_remove_key(a_module, "displayName");
            if (!AddString(a_document, a_module, "displayName", name))
            {
                a_error = "The module could not be renamed.";
                return false;
            }
            return true;
        }

        bool AddDescriptionFields(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_module,
            const std::string_view a_header,
            const std::string_view a_text,
            const bool a_defaultOpen)
        {
            return AddString(a_document, a_module, "type", "description") &&
                   (a_header.empty() || AddString(a_document, a_module, "header", a_header)) &&
                   AddString(a_document, a_module, "text", a_text) &&
                   (!a_defaultOpen || yyjson_mut_obj_add_bool(a_document, a_module, "defaultOpen", true));
        }

        yyjson_mut_val* StringList(yyjson_mut_doc* a_document, const std::vector<std::string>& a_values)
        {
            auto* result = yyjson_mut_arr(a_document);
            for (const auto& value : a_values)
                if (!yyjson_mut_arr_add_strncpy(a_document, result, value.data(), value.size())) return nullptr;
            return result;
        }

        bool AddFilter(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_parent,
            const std::string_view a_name,
            const Filter& a_filter)
        {
            auto* filter = yyjson_mut_obj(a_document);
            auto* formIDs = StringList(a_document, a_filter.formIDs);
            auto* contains = StringList(a_document, a_filter.contains);
            auto* locationTypes = StringList(a_document, a_filter.locationTypes);
            auto* multiLocationExceptions = StringList(a_document, a_filter.multiLocationExceptions);
            return filter && formIDs && contains && locationTypes && multiLocationExceptions &&
                   yyjson_mut_obj_add_val(a_document, filter, "formIDs", formIDs) &&
                   yyjson_mut_obj_add_val(a_document, filter, "contains", contains) &&
                   (a_filter.locationTypes.empty() ||
                       yyjson_mut_obj_add_val(a_document, filter, "locationTypes", locationTypes)) &&
                   (a_filter.multiLocationExceptions.empty() ||
                       yyjson_mut_obj_add_val(
                           a_document,
                           filter,
                           "multiLocationExceptions",
                           multiLocationExceptions)) &&
                   yyjson_mut_obj_add_val(a_document, a_parent, a_name.data(), filter);
        }

        double NormalizeSliderNumber(const double a_value)
        {
            constexpr double precision = 1000.0;
            const auto result = std::round(a_value * precision) / precision;
            return result == 0.0 ? 0.0 : result;
        }

        bool AddCustomLinks(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_parent,
            const CustomLinks& a_links)
        {
            auto* links = yyjson_mut_obj(a_document);
            if (!links) return false;
            for (const auto& [target, link] : a_links)
            {
                const auto& [source, scale] = link;
                auto* value = yyjson_mut_arr(a_document);
                if (!value ||
                    !yyjson_mut_arr_add_strncpy(a_document, value, source.data(), source.size()) ||
                    !yyjson_mut_arr_add_real(a_document, value, NormalizeSliderNumber(scale)) ||
                    !yyjson_mut_obj_add_val(a_document, links, target.c_str(), value))
                    return false;
            }
            return yyjson_mut_obj_add_val(a_document, a_parent, "customLinks", links);
        }

        yyjson_mut_val* BuildControl(yyjson_mut_doc* a_document, const Definition& a_definition)
        {
            auto* control = yyjson_mut_obj(a_document);
            if (!control || !AddString(a_document, control, "type", "slider") ||
                !AddString(a_document, control, "id", a_definition.id) ||
                !AddString(a_document, control, "label", a_definition.label))
                return nullptr;
            if (!a_definition.filtered && a_definition.settings.size() == 1)
            {
                if (!AddString(a_document, control, "setting", a_definition.settings.front().setting)) return nullptr;
            }
            else
            {
                auto* settings = yyjson_mut_arr(a_document);
                if (!settings) return nullptr;
                for (const auto& target : a_definition.settings)
                {
                    if (!a_definition.filtered)
                    {
                        if (!yyjson_mut_arr_add_strncpy(
                                a_document,
                                settings,
                                target.setting.data(),
                                target.setting.size()))
                            return nullptr;
                        continue;
                    }
                    auto* setting = yyjson_mut_obj(a_document);
                    if (!setting || !AddString(a_document, setting, "setting", target.setting) ||
                        (a_definition.filtered &&
                            !yyjson_mut_obj_add_real(
                                a_document,
                                setting,
                                "scale",
                                NormalizeSliderNumber(target.scale))) ||
                        !yyjson_mut_arr_append(settings, setting))
                        return nullptr;
                }
                if (!yyjson_mut_obj_add_val(a_document, control, "settings", settings)) return nullptr;
            }

            if (a_definition.customLinks && !AddCustomLinks(a_document, control, *a_definition.customLinks))
                return nullptr;

            if (a_definition.ignoreProfileFilters &&
                !yyjson_mut_obj_add_bool(a_document, control, "ignoreProfileFilters", true))
                return nullptr;
            if (a_definition.invert && !yyjson_mut_obj_add_bool(a_document, control, "invert", true)) return nullptr;
            if (a_definition.useTimes)
            {
                static constexpr std::array names{ "sunrise", "day", "sunset", "night" };
                auto* times = yyjson_mut_arr(a_document);
                if (!times) return nullptr;
                for (std::size_t index = 0; index < names.size(); ++index)
                    if (a_definition.times[index] && !yyjson_mut_arr_add_strcpy(a_document, times, names[index])) return nullptr;
                if (!yyjson_mut_obj_add_val(a_document, control, "times", times)) return nullptr;
            }

            const auto hasRecordFilter =
                (a_definition.filtered && a_definition.filterDomain != FilterDomain::weather) ||
                !a_definition.include.formIDs.empty() || !a_definition.include.contains.empty() ||
                !a_definition.include.locationTypes.empty() ||
                !a_definition.include.multiLocationExceptions.empty() ||
                !a_definition.exclude.formIDs.empty() || !a_definition.exclude.contains.empty() ||
                !a_definition.exclude.locationTypes.empty() ||
                !a_definition.exclude.multiLocationExceptions.empty();
            if (hasRecordFilter)
            {
                auto* filter = yyjson_mut_obj(a_document);
                if (!filter || !AddFilter(a_document, filter, "include", a_definition.include) ||
                    !AddFilter(a_document, filter, "exclude", a_definition.exclude) ||
                    !yyjson_mut_obj_add_val(
                        a_document,
                        control,
                        a_definition.filterDomain == FilterDomain::lightingTemplate ?
                            "lightingTemplateFilter" :
                        a_definition.filterDomain == FilterDomain::baseLight ?
                            "baseLightFilter" :
                        a_definition.filterDomain == FilterDomain::baseObject ?
                            "baseObjectFilter" :
                            "weatherFilter",
                        filter))
                    return nullptr;
            }

            if (a_definition.hueScales)
            {
                const auto& scales = *a_definition.hueScales;
                auto* value = yyjson_mut_obj(a_document);
                const auto addScale = [&](const char* a_key, const double a_scale)
                {
                    return yyjson_mut_obj_add_real(
                        a_document,
                        value,
                        a_key,
                        NormalizeSliderNumber(a_scale));
                };
                if (!value || !addScale("red", scales.red) ||
                    !addScale("orange", scales.orange) ||
                    !addScale("yellow", scales.yellow) ||
                    !addScale("green", scales.green) ||
                    !addScale("teal", scales.teal) ||
                    !addScale("blue", scales.blue) ||
                    !addScale("magenta", scales.magenta) ||
                    !yyjson_mut_obj_add_val(a_document, control, "hueScales", value))
                    return nullptr;
            }

            const auto addNumber = [&](const char* a_key, const std::optional<double> a_value)
            {
                return !a_value || yyjson_mut_obj_add_real(
                                       a_document,
                                       control,
                                       a_key,
                                       NormalizeSliderNumber(*a_value));
            };
            if (!addNumber("min", a_definition.minimum) || !addNumber("max", a_definition.maximum) ||
                !addNumber("step", a_definition.step) ||
                !addNumber("width", a_definition.width))
                return nullptr;
            if (!a_definition.format.empty() && !AddString(a_document, control, "format", a_definition.format)) return nullptr;
            return control;
        }

        bool ValidID(const std::string_view a_id)
        {
            return !a_id.empty() && std::ranges::all_of(a_id, [](const unsigned char a_character)
            {
                return std::isalnum(a_character) != 0 || a_character == '_' || a_character == '-';
            });
        }

        const SliderSettingCatalog::Entry* CatalogEntry(const std::string_view a_setting)
        {
            return SliderSettingCatalog::Find(Trim(std::string(a_setting)));
        }

        bool ValidateCustomLinks(
            const Definition& a_definition,
            const std::span<const SliderSettingCatalog::Entry* const> a_entries,
            std::string& a_error)
        {
            if (!a_definition.customLinks) return true;

            const bool weather = a_definition.filtered &&
                                 a_definition.filterDomain == FilterDomain::weather &&
                                 std::ranges::all_of(a_entries, [](const auto* a_entry)
                                 { return a_entry->linkable; });
            const bool lighting =
                (a_definition.filtered && a_definition.filterDomain == FilterDomain::lightingTemplate &&
                    std::ranges::all_of(a_entries, [](const auto* a_entry)
                        { return a_entry->filterOperation == SliderSettingCatalog::FilterOperation::brightness; })) ||
                (!a_definition.filtered && std::ranges::all_of(a_definition.settings, [](const auto& a_target)
                    { return IsLightingLinkableSetting(a_target.setting); }));
            if (!weather && !lighting)
            {
                a_error = "Custom links apply only to linkable Weather and Lighting sliders.";
                return false;
            }

            static constexpr std::array weatherFields{
                std::string_view{ "ambient" }, std::string_view{ "sunlight" },
                std::string_view{ "effectLighting" }, std::string_view{ "fogFar" },
                std::string_view{ "fogNear" }, std::string_view{ "water" },
                std::string_view{ "skyStatics" }, std::string_view{ "skyUpper" },
                std::string_view{ "skyLower" }, std::string_view{ "horizon" },
                std::string_view{ "sun" }, std::string_view{ "sunGlare" },
                std::string_view{ "moonGlare" }, std::string_view{ "stars" },
                std::string_view{ "cloudLayers" }, std::string_view{ "volumetricLighting" },
            };
            static constexpr std::array lightingFields{
                std::string_view{ "ambient" }, std::string_view{ "directional" },
                std::string_view{ "ambientColors" }, std::string_view{ "fogFar" },
                std::string_view{ "fogNear" },
            };
            const auto validField = [&](const std::string_view a_field)
            {
                if (weather)
                    return std::ranges::any_of(weatherFields, [&](const auto a_candidate)
                        { return IEquals(a_candidate, a_field); });
                return std::ranges::any_of(lightingFields, [&](const auto a_candidate)
                    { return IEquals(a_candidate, a_field); });
            };
            for (const auto& [target, link] : *a_definition.customLinks)
            {
                const auto& [source, scale] = link;
                if (!validField(target) || !validField(source) || IEquals(target, source) || !std::isfinite(scale))
                {
                    a_error = "Every custom link must use two different valid fields and a finite scale.";
                    return false;
                }
            }

            for (const auto& [start, unused] : *a_definition.customLinks)
            {
                (void) unused;
                std::vector<std::string> visited;
                auto current = start;
                while (true)
                {
                    const auto link = std::ranges::find_if(
                        *a_definition.customLinks,
                        [&](const auto& a_entry) { return IEquals(a_entry.first, current); });
                    if (link == a_definition.customLinks->end()) break;
                    if (std::ranges::any_of(visited, [&](const auto& a_field)
                            { return IEquals(a_field, current); }))
                    {
                        a_error = "Custom links cannot contain a cycle.";
                        return false;
                    }
                    visited.push_back(current);
                    current = std::get<0>(link->second);
                }
            }
            return true;
        }

        bool Validate(const Definition& a_definition, std::string& a_error)
        {
            if (Trim(a_definition.label).empty())
            {
                a_error = "Enter a slider name.";
                return false;
            }
            if (!ValidID(a_definition.id))
            {
                a_error = "The slider name could not be converted to a valid ID.";
                return false;
            }
            if (a_definition.settings.empty() || std::ranges::any_of(a_definition.settings, [](const auto& a_target)
                    { return Trim(a_target.setting).empty() || !std::isfinite(a_target.scale); }))
            {
                a_error = "Add at least one valid setting to the slider.";
                return false;
            }
            std::vector<const SliderSettingCatalog::Entry*> entries;
            entries.reserve(a_definition.settings.size());
            for (const auto& target : a_definition.settings)
            {
                const auto* entry = CatalogEntry(target.setting);
                if (!entry)
                {
                    a_error = "The slider contains a setting path that TuningUtil does not support.";
                    return false;
                }
                entries.push_back(entry);
            }
            if (!ValidateCustomLinks(a_definition, entries, a_error)) return false;
            if (a_definition.ignoreProfileFilters && !a_definition.filtered)
            {
                a_error = "Ignore Profile Filters applies only to sliders with record filters.";
                return false;
            }
            if (a_definition.ignoreProfileFilters &&
                (a_definition.filterDomain == FilterDomain::baseLight ||
                    a_definition.filterDomain == FilterDomain::baseObject))
            {
                a_error = "Base Light and Base Object sliders do not have a profile-level record filter to ignore.";
                return false;
            }
            const auto objectEffectLighting = std::ranges::all_of(
                entries,
                [](const auto* a_entry)
                {
                    return a_entry->path == "objectEffectLighting.emissiveMultiplier" ||
                           a_entry->path == "objectEffectLighting.baseColorScale";
                });
            if (objectEffectLighting &&
                (!a_definition.filtered || a_definition.filterDomain != FilterDomain::baseObject))
            {
                a_error = "Object Effect Lighting sliders require a Base Object filter.";
                return false;
            }
            if (a_definition.filtered)
            {
                if (a_definition.filterDomain == FilterDomain::baseObject)
                {
                    if (!objectEffectLighting)
                    {
                        a_error = "Base Object filters support only Object Effect Lighting settings.";
                        return false;
                    }
                    if (a_definition.useTimes || a_definition.hueScales)
                    {
                        a_error = "Time filters and saturation scales do not apply to Base Object filters.";
                        return false;
                    }
                }
                else if (a_definition.filterDomain == FilterDomain::baseLight)
                {
                    std::optional<SliderSettingCatalog::FilterOperation> operation;
                    for (const auto* entry : entries)
                    {
                        if (entry->domain != SliderSettingCatalog::Domain::lighting ||
                            !entry->path.starts_with("pointLights.") ||
                            !SliderSettingCatalog::IsFilteredOperation(entry->filterOperation))
                        {
                            a_error = "Base Light filters support only Point Lights settings.";
                            return false;
                        }
                        if (operation && operation != entry->filterOperation)
                        {
                            a_error = "Every setting in a filtered slider must use the same operation.";
                            return false;
                        }
                        operation = entry->filterOperation;
                    }
                    if (a_definition.useTimes || a_definition.hueScales)
                    {
                        a_error = "Time filters and saturation scales do not apply to Base Light filters.";
                        return false;
                    }
                }
                else if (a_definition.filterDomain == FilterDomain::lightingTemplate)
                {
                    std::optional<SliderSettingCatalog::FilterOperation> operation;
                    for (const auto* entry : entries)
                    {
                        if (entry->domain != SliderSettingCatalog::Domain::lighting ||
                            (!entry->path.starts_with("lightBrightnessMultiplier.") &&
                                entry->path != "lightFogPowerMultiplier" &&
                                entry->path != "lightFogMaxMultiplier") ||
                            (entry->filterOperation != SliderSettingCatalog::FilterOperation::brightness &&
                                entry->filterOperation != SliderSettingCatalog::FilterOperation::fogPower &&
                                entry->filterOperation != SliderSettingCatalog::FilterOperation::fogStrength))
                        {
                            a_error = "Lighting Template filters support only Lighting brightness, Fog Power, and Fog Strength settings.";
                            return false;
                        }
                        if (operation && operation != entry->filterOperation)
                        {
                            a_error = "Every setting in a filtered slider must use the same operation.";
                            return false;
                        }
                        operation = entry->filterOperation;
                    }
                    if (a_definition.useTimes || a_definition.hueScales)
                    {
                        a_error = "Time filters and saturation scales apply only to filtered weather sliders.";
                        return false;
                    }
                }
                else
                {
                    std::optional<SliderSettingCatalog::FilterOperation> operation;
                    for (const auto* entry : entries)
                    {
                        if (entry->domain != SliderSettingCatalog::Domain::weather ||
                            !SliderSettingCatalog::IsFilteredOperation(entry->filterOperation))
                        {
                            a_error = "Filtered sliders support only weather brightness, saturation, hue-shift, and volumetric-lighting intensity settings.";
                            return false;
                        }
                        if (operation && operation != entry->filterOperation)
                        {
                            a_error = "Every setting in a filtered slider must use the same operation.";
                            return false;
                        }
                        operation = entry->filterOperation;
                    }
                    if (a_definition.hueScales && *operation != SliderSettingCatalog::FilterOperation::saturation)
                    {
                        a_error = "Slider-specific saturation scales are supported only by filtered saturation sliders.";
                        return false;
                    }
                }
            }
            else if (a_definition.hueScales)
            {
                a_error = "Slider-specific saturation scales require a filtered weather slider.";
                return false;
            }
            if (a_definition.hueScales)
            {
                const auto& scales = *a_definition.hueScales;
                const std::array values{
                    scales.red, scales.orange, scales.yellow, scales.green,
                    scales.teal, scales.blue, scales.magenta,
                };
                if (std::ranges::any_of(values, [](const double a_value) { return !std::isfinite(a_value); }))
                {
                    a_error = "Every slider-specific saturation scale must be a finite number.";
                    return false;
                }
            }
            if (a_definition.useTimes && !std::ranges::any_of(a_definition.times, std::identity{}))
            {
                a_error = "Select at least one time of day or disable the time filter.";
                return false;
            }
            if (a_definition.minimum && a_definition.maximum && *a_definition.minimum >= *a_definition.maximum)
            {
                a_error = "The slider minimum must be lower than its maximum.";
                return false;
            }
            if (a_definition.step && *a_definition.step < 0.0)
            {
                a_error = "The slider step cannot be negative.";
                return false;
            }
            return true;
        }

        std::optional<std::string> ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file) return std::nullopt;
            std::string text(std::istreambuf_iterator<char>(file), {});
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom)) text.erase(0, bom.size());
            return text;
        }

        bool CopyFileContents(
            const std::filesystem::path& a_source,
            const std::filesystem::path& a_destination,
            std::string& a_error)
        {
            std::ifstream source(a_source, std::ios::binary);
            if (!source)
            {
                a_error = std::format("The source profile file {} could not be opened.", a_source.string());
                return false;
            }
            std::ofstream destination(a_destination, std::ios::binary | std::ios::trunc);
            if (!destination)
            {
                a_error = std::format("The copied profile file {} could not be created.", a_destination.string());
                return false;
            }

            std::array<char, 64 * 1024> buffer{};
            while (source)
            {
                source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto length = source.gcount();
                if (length > 0) destination.write(buffer.data(), length);
            }
            if (!source.eof())
            {
                a_error = std::format("The source profile file {} could not be read.", a_source.string());
                return false;
            }
            destination.flush();
            if (!destination)
            {
                a_error = std::format("The copied profile file {} could not be written.", a_destination.string());
                return false;
            }
            return true;
        }

        bool ValidProfileName(const std::string_view a_name)
        {
            if (a_name.empty() || a_name == "." || a_name == ".." ||
                a_name.ends_with(' ') || a_name.ends_with('.'))
                return false;
            if (std::ranges::any_of(a_name, [](const unsigned char a_character)
                {
                    constexpr std::string_view invalid = "<>:\"/\\|?*";
                    return a_character < 32 || invalid.contains(static_cast<char>(a_character));
                }))
                return false;

            auto deviceName = std::string(a_name.substr(0, a_name.find('.')));
            std::ranges::transform(deviceName, deviceName.begin(), [](const unsigned char a_character)
                { return static_cast<char>(std::toupper(a_character)); });
            static constexpr std::array reserved{ "CON", "PRN", "AUX", "NUL" };
            if (std::ranges::contains(reserved, std::string_view(deviceName))) return false;
            if (deviceName.size() == 4 &&
                (deviceName.starts_with("COM") || deviceName.starts_with("LPT")) &&
                deviceName[3] >= '1' && deviceName[3] <= '9')
                return false;
            return true;
        }

        bool KnownSliderKey(const std::string_view a_key)
        {
            static constexpr std::array keys{
                "type", "id", "label", "customLinks", "hueScales", "setting", "settings",
                "ignoreProfileFilters", "invert", "times", "weatherFilter", "lightingTemplateFilter", "baseLightFilter", "baseObjectFilter", "min", "max", "step", "width", "format",
            };
            return std::ranges::any_of(keys, [&](const auto a_known) { return IEquals(a_key, a_known); });
        }

        bool CopyUnknownSliderMembers(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_target,
            yyjson_val* a_source)
        {
            if (!yyjson_is_obj(a_source)) return true;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* key = nullptr;
            yyjson_val* value = nullptr;
            yyjson_obj_foreach(a_source, index, maximum, key, value)
            {
                const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
                if (KnownSliderKey(name)) continue;
                auto* copiedKey = yyjson_mut_strncpy(a_document, name.data(), name.size());
                auto* copiedValue = yyjson_val_mut_copy(a_document, value);
                if (!copiedKey || !copiedValue || !yyjson_mut_obj_add(a_target, copiedKey, copiedValue)) return false;
            }
            return true;
        }

        bool WriteDocument(
            const std::filesystem::path& a_path,
            yyjson_mut_doc* a_document,
            std::string& a_error)
        {
            std::size_t length = 0;
            auto* data = yyjson_mut_write(a_document, YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
            if (!data)
            {
                a_error = "The updated JSON could not be serialized.";
                return false;
            }
            std::string output(data, length);
            std::free(data);

            auto temporaryPath = a_path;
            temporaryPath += ".tmp";
            {
                std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
                file << output << '\n';
                if (!file)
                {
                    file.close();
                    std::error_code removeError;
                    std::filesystem::remove(temporaryPath, removeError);
                    a_error = "The temporary JSON file could not be written.";
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
            a_error = std::format("The JSON file could not be replaced: {}", moveError.message());
            return false;
        }

        bool ReplaceRootStringMember(
            const std::filesystem::path& a_path,
            const std::string_view a_key,
            const std::string_view a_value,
            std::string& a_error)
        {
            const auto text = ReadText(a_path);
            Document source(text ?
                                yyjson_read(
                                    const_cast<char*>(text->data()),
                                    text->size(),
                                    YYJSON_READ_NOFLAG) :
                                nullptr);
            auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
            if (!yyjson_is_obj(sourceRoot))
            {
                a_error = std::format("{} does not contain a JSON object.", a_path.filename().string());
                return false;
            }

            MutableDocument document(yyjson_mut_doc_new(nullptr));
            auto* root = document ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
            auto* value = document ? yyjson_mut_strncpy(document.get(), a_value.data(), a_value.size()) : nullptr;
            if (!document || !root || !value)
            {
                a_error = std::format("{} could not be copied for editing.", a_path.filename().string());
                return false;
            }
            yyjson_mut_doc_set_root(document.get(), root);
            yyjson_mut_obj_remove_key(root, a_key.data());
            if (!yyjson_mut_obj_add_val(document.get(), root, a_key.data(), value))
            {
                a_error = std::format("{} could not be updated.", a_path.filename().string());
                return false;
            }
            return WriteDocument(a_path, document.get(), a_error);
        }

        bool RemoveRootMember(
            const std::filesystem::path& a_path,
            const std::string_view a_key,
            std::string& a_error)
        {
            const auto text = ReadText(a_path);
            Document source(text ?
                                yyjson_read(
                                    const_cast<char*>(text->data()),
                                    text->size(),
                                    YYJSON_READ_NOFLAG) :
                                nullptr);
            auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
            if (!yyjson_is_obj(sourceRoot))
            {
                a_error = std::format("{} does not contain a JSON object.", a_path.filename().string());
                return false;
            }

            MutableDocument document(yyjson_mut_doc_new(nullptr));
            auto* root = document ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
            if (!document || !root)
            {
                a_error = std::format("{} could not be copied for editing.", a_path.filename().string());
                return false;
            }
            yyjson_mut_doc_set_root(document.get(), root);
            yyjson_mut_obj_remove_key(root, a_key.data());
            return WriteDocument(a_path, document.get(), a_error);
        }
    }  // namespace

    std::optional<ProfilePluginGating> LoadProfilePluginGating(
        const std::filesystem::path& a_path,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document document(text ?
                              yyjson_read(
                                  const_cast<char*>(text->data()),
                                  text->size(),
                                  YYJSON_READ_NOFLAG) :
                              nullptr);
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        if (!yyjson_is_obj(root))
        {
            a_error = "profileSettings.json does not contain a JSON object.";
            return std::nullopt;
        }
        auto disabledProfiles = StringArray(root, "DisableProfile");
        if (const auto singleProfile = StringMember(root, "DisableProfile");
            singleProfile && !Trim(*singleProfile).empty())
        {
            disabledProfiles.push_back(Trim(*singleProfile));
        }
        return ProfilePluginGating{
            .dependencies = StringArray(root, "PluginDependency"),
            .disabledProfiles = std::move(disabledProfiles),
        };
    }

    bool SaveProfilePluginGating(
        const std::filesystem::path& a_path,
        const ProfilePluginGating& a_gating,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ?
                            yyjson_read(
                                const_cast<char*>(text->data()),
                                text->size(),
                                YYJSON_READ_NOFLAG) :
                            nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot))
        {
            a_error = "profileSettings.json does not contain a JSON object.";
            return false;
        }

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!document || !root)
        {
            a_error = "profileSettings.json could not be copied for editing.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        if (!ReplaceStringArray(document.get(), root, "PluginDependency", a_gating.dependencies) ||
            !ReplaceStringArray(document.get(), root, "DisableProfile", a_gating.disabledProfiles))
        {
            a_error = "The profile plugin gating could not be updated.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    std::optional<std::string> LoadAmbientAnchorWeather(
        const std::filesystem::path& a_path,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document document(text ?
                              yyjson_read(
                                  const_cast<char*>(text->data()),
                                  text->size(),
                                  YYJSON_READ_NOFLAG) :
                              nullptr);
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        if (!yyjson_is_obj(root))
        {
            a_error = "profileSettings.json does not contain a JSON object.";
            return std::nullopt;
        }
        auto weather = Trim(StringMember(root, "ambientAnchorWeather").value_or("SkyrimClear"));
        return weather.empty() ? std::string("SkyrimClear") : std::move(weather);
    }

    bool SaveAmbientAnchorWeather(
        const std::filesystem::path& a_path,
        const std::string_view a_weather,
        std::string& a_error)
    {
        a_error.clear();
        const auto weather = Trim(std::string(a_weather));
        if (weather.empty())
        {
            a_error = "Select an ambient anchor weather.";
            return false;
        }
        if (IEquals(weather, "SkyrimClear"))
        {
            return RemoveRootMember(a_path, "ambientAnchorWeather", a_error);
        }
        return ReplaceRootStringMember(a_path, "ambientAnchorWeather", weather, a_error);
    }

    std::vector<Page> Load(const std::filesystem::path& a_path, std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document document(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        auto* pages = yyjson_is_obj(root) ? yyjson_obj_get(root, "pages") : nullptr;
        if (NumberMember(root, "schemaVersion").value_or(0.0) != 1.0)
        {
            a_error = "The menu file does not use schemaVersion 1.";
            return {};
        }
        if (!yyjson_is_arr(pages))
        {
            a_error = "The menu file does not contain a pages array.";
            return {};
        }

        std::vector<Page> result;
        std::size_t pageIndex = 0;
        std::size_t pageMaximum = 0;
        yyjson_val* pageValue = nullptr;
        yyjson_arr_foreach(pages, pageIndex, pageMaximum, pageValue)
        {
            Page page{
                .title = StringMember(pageValue, "title").value_or(std::format("Page {}", pageIndex + 1)),
                .advanced = BooleanMember(pageValue, "advanced"),
            };
            auto* modules = yyjson_obj_get(pageValue, "modules");
            if (yyjson_is_arr(modules))
            {
                std::size_t controlIndex = 0;
                std::size_t controlMaximum = 0;
                yyjson_val* control = nullptr;
                yyjson_arr_foreach(modules, controlIndex, controlMaximum, control)
                {
                    const auto kind = StringMember(control, "type");
                    if (kind && IEquals(*kind, "slider"))
                        page.sliders.push_back({ controlIndex, ReadDefinition(control) });
                }
            }
            result.push_back(std::move(page));
        }
        return result;
    }

    bool CreateProfile(
        const std::filesystem::path& a_tuningRoot,
        const std::string& a_profileName,
        std::string& a_error,
        const std::filesystem::path& a_sourceProfile,
        const std::optional<ProfileTemplate> a_profileTemplate)
    {
        a_error.clear();
        const auto profileName = Trim(a_profileName);
        if (!ValidProfileName(profileName))
        {
            a_error = "Enter a valid profile name that can be used as a Windows folder name.";
            return false;
        }

        const auto copying = !a_sourceProfile.empty();
        if (copying && a_profileTemplate)
        {
            a_error = "Select either a profile template or an existing profile to copy.";
            return false;
        }
        if (!copying && !a_profileTemplate)
        {
            a_error = "Select a profile template or an existing profile to copy.";
            return false;
        }
        const auto templateName = a_profileTemplate == ProfileTemplate::weather ?
                                      "skseMenuWeatherTemplate.json" :
                                      "skseMenuLightingTemplate.json";
        const auto menuSource = copying ? a_sourceProfile / "skseMenu.json" :
                                          a_tuningRoot / templateName;
        const auto menuText = ReadText(menuSource);
        Document menuDocument(menuText ?
                                  yyjson_read(
                                      const_cast<char*>(menuText->data()),
                                      menuText->size(),
                                      YYJSON_READ_NOFLAG) :
                                  nullptr);
        auto* menuRoot = menuDocument ? yyjson_doc_get_root(menuDocument.get()) : nullptr;
        if (!yyjson_is_obj(menuRoot) ||
            NumberMember(menuRoot, "schemaVersion").value_or(0.0) != 1.0 ||
            !yyjson_is_arr(yyjson_obj_get(menuRoot, "pages")))
        {
            a_error = copying ?
                          "The source profile does not contain a valid skseMenu.json." :
                          "The selected profile template is missing or does not contain a valid pages array.";
            return false;
        }
        if (copying)
        {
            const auto sourceAttributes = ::GetFileAttributesW(a_sourceProfile.c_str());
            if (sourceAttributes == INVALID_FILE_ATTRIBUTES ||
                (sourceAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                a_error = "The source profile folder is unavailable.";
                return false;
            }
            const auto settingsText = ReadText(a_sourceProfile / "profileSettings.json");
            Document settingsDocument(settingsText ?
                                          yyjson_read(
                                              const_cast<char*>(settingsText->data()),
                                              settingsText->size(),
                                              YYJSON_READ_NOFLAG) :
                                          nullptr);
            if (!settingsDocument || !yyjson_is_obj(yyjson_doc_get_root(settingsDocument.get())))
            {
                a_error = "The source profile does not contain a valid profileSettings.json.";
                return false;
            }
            std::string presetError;
            if (!PresetCatalog::Read(a_sourceProfile / PresetCatalog::kFileName, presetError))
            {
                a_error = "The source profile does not contain a valid presets.json.";
                return false;
            }
        }

        const auto profileDirectory = a_tuningRoot / profileName;
        std::error_code filesystemError;
        const auto existingAttributes = ::GetFileAttributesW(profileDirectory.c_str());
        if (existingAttributes != INVALID_FILE_ATTRIBUTES)
        {
            a_error = "A profile folder with that name already exists.";
            return false;
        }
        const auto attributeError = ::GetLastError();
        if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
        {
            a_error = std::format(
                "The profile folder could not be checked: {}",
                std::system_category().message(static_cast<int>(attributeError)));
            return false;
        }
        if (!std::filesystem::create_directory(profileDirectory, filesystemError))
        {
            a_error = filesystemError ?
                          std::format("The profile folder could not be created: {}", filesystemError.message()) :
                          "The profile folder could not be created.";
            return false;
        }

        const auto removeIncompleteProfile = [&]
        {
            std::error_code ignored;
            std::filesystem::remove_all(profileDirectory, ignored);
        };

        if (copying)
        {
            const auto copyFailure = [&](const std::string_view a_reason)
            {
                removeIncompleteProfile();
                a_error = std::string(a_reason);
                return false;
            };
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator iterator(a_sourceProfile, options, filesystemError), end;
                iterator != end && !filesystemError;
                iterator.increment(filesystemError))
            {
                const auto filename = iterator->path().filename().string();
                if (IEquals(filename, "skseMenu.edit.json") ||
                    IEquals(filename, "skseMenu.commit.json") ||
                    iterator->path().extension() == ".tmp")
                {
                    continue;
                }
                const auto relative = iterator->path().lexically_relative(a_sourceProfile);
                if (relative.empty() || *relative.begin() == "..")
                {
                    filesystemError = std::make_error_code(std::errc::invalid_argument);
                    break;
                }
                const auto destination = profileDirectory / relative;
                const auto attributes = ::GetFileAttributesW(iterator->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    filesystemError.assign(static_cast<int>(::GetLastError()), std::system_category());
                    break;
                }
                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    std::filesystem::create_directories(destination, filesystemError);
                }
                else
                {
                    std::filesystem::create_directories(destination.parent_path(), filesystemError);
                    if (!filesystemError && !CopyFileContents(iterator->path(), destination, a_error))
                    {
                        const auto reason = a_error;
                        return copyFailure(reason);
                    }
                }
            }
            if (filesystemError)
            {
                return copyFailure(std::format("The source profile could not be copied: {}", filesystemError.message()));
            }
            if (!ReplaceRootStringMember(profileDirectory / "profileSettings.json", "profile", profileName, a_error) ||
                !ReplaceRootStringMember(
                    profileDirectory / "skseMenu.json",
                    "title",
                    profileName,
                    a_error))
            {
                const auto reason = a_error;
                return copyFailure(reason);
            }
            return true;
        }

        {
            std::ofstream menuFile(profileDirectory / "skseMenu.json", std::ios::binary | std::ios::trunc);
            menuFile.write(menuText->data(), static_cast<std::streamsize>(menuText->size()));
            if (!menuFile)
            {
                menuFile.close();
                removeIncompleteProfile();
                a_error = "The new profile's skseMenu.json could not be written.";
                return false;
            }
        }
        if (!ReplaceRootStringMember(
                profileDirectory / "skseMenu.json",
                "title",
                profileName,
                a_error))
        {
            const auto reason = a_error;
            removeIncompleteProfile();
            a_error = reason;
            return false;
        }
        {
            std::ofstream settingsFile(profileDirectory / "profileSettings.json", std::ios::binary | std::ios::trunc);
            settingsFile << std::format(
                "{{\n  \"profile\": \"{}\",\n  \"EnableProfile\": true\n}}\n",
                profileName);
            if (!settingsFile)
            {
                settingsFile.close();
                removeIncompleteProfile();
                a_error = "The new profile's profileSettings.json could not be written.";
                return false;
            }
        }
        if (!PresetCatalog::Write(
                profileDirectory / PresetCatalog::kFileName,
                PresetCatalog::Catalog{},
                a_error))
        {
            removeIncompleteProfile();
            return false;
        }
        return true;
    }

    std::optional<std::size_t> CreatePage(
        const std::filesystem::path& a_path,
        const std::string& a_title,
        std::string& a_error)
    {
        a_error.clear();
        const auto title = Trim(a_title);
        if (title.empty())
        {
            a_error = "Enter a page name.";
            return std::nullopt;
        }

        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        auto* sourcePages = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "pages") : nullptr;
        if (!yyjson_is_arr(sourcePages))
        {
            a_error = "The menu file does not contain a pages array.";
            return std::nullopt;
        }

        std::size_t index = 0;
        std::size_t maximum = 0;
        yyjson_val* sourcePage = nullptr;
        yyjson_arr_foreach(sourcePages, index, maximum, sourcePage)
        {
            if (const auto existing = StringMember(sourcePage, "title"); existing && IEquals(*existing, title))
            {
                a_error = "A page with that name already exists.";
                return std::nullopt;
            }
        }
        const auto pageIndex = yyjson_arr_size(sourcePages);

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        auto* pages = root ? yyjson_mut_obj_get(root, "pages") : nullptr;
        auto* page = yyjson_mut_obj(document.get());
        auto* modules = yyjson_mut_arr(document.get());
        if (!root || !yyjson_mut_is_arr(pages) || !page || !modules ||
            !AddString(document.get(), page, "title", title) ||
            !yyjson_mut_obj_add_val(document.get(), page, "modules", modules) ||
            !yyjson_mut_arr_append(pages, page))
        {
            a_error = "The new page JSON could not be created.";
            return std::nullopt;
        }
        if (!SetProfilePageLast(document.get(), root))
        {
            a_error = "The Profile page could not be kept last.";
            return std::nullopt;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        if (!WriteDocument(a_path, document.get(), a_error)) return std::nullopt;
        return pageIndex;
    }

    bool AddModule(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::string& a_kind,
        const std::string& a_label,
        const std::string& a_setting,
        std::string& a_error,
        const bool a_defaultOpen)
    {
        a_error.clear();
        const auto kind = Trim(a_kind);
        if (kind.empty())
        {
            a_error = "Select a module to add.";
            return false;
        }

        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        auto* module = yyjson_mut_obj(document.get());
        const auto labelKey = kind == "text" || kind == "separatorText" ||
                                      kind == "dropBoxStart" ?
                                  "label" : "header";
        const auto dropBoxStart = kind == "dropBoxStart";
        if (!yyjson_mut_is_arr(modules) || !module || !AddString(document.get(), module, "type", kind) ||
            (kind != "boxStart" && !a_label.empty() && !AddString(document.get(), module, labelKey, a_label)) ||
            (!a_setting.empty() && !AddString(document.get(), module, "setting", a_setting)) ||
            (dropBoxStart && !yyjson_mut_obj_add_bool(document.get(), module, "defaultOpen", a_defaultOpen)) ||
            !yyjson_mut_arr_append(modules, module))
        {
            a_error = "The module could not be added to this page.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool AddDescriptionModule(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::string& a_header,
        const std::string& a_text,
        const bool a_defaultOpen,
        std::string& a_error)
    {
        a_error.clear();
        const auto header = Trim(a_header);
        const auto description = Trim(a_text);
        if (description.empty())
        {
            a_error = "Enter a description.";
            return false;
        }

        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        auto* module = yyjson_mut_obj(document.get());
        if (!yyjson_mut_is_arr(modules) || !module ||
            !AddDescriptionFields(document.get(), module, header, description, a_defaultOpen) ||
            !yyjson_mut_arr_append(modules, module))
        {
            a_error = "The description could not be added to this page.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool MoveModule(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::size_t a_controlIndex,
        const int a_direction,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        const auto count = yyjson_mut_is_arr(modules) ? yyjson_mut_arr_size(modules) : 0;
        const auto destination = static_cast<std::ptrdiff_t>(a_controlIndex) + a_direction;
        if (a_controlIndex >= count || destination < 0 || destination >= static_cast<std::ptrdiff_t>(count))
        {
            a_error = "The module cannot move farther in that direction.";
            return false;
        }
        auto* value = yyjson_mut_arr_remove(modules, a_controlIndex);
        if (!value || !yyjson_mut_arr_insert(modules, value, static_cast<std::size_t>(destination)))
        {
            a_error = "The module order could not be changed.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool RemoveModule(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::size_t a_controlIndex,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        if (!yyjson_mut_is_arr(modules) || !yyjson_mut_arr_remove(modules, a_controlIndex))
        {
            a_error = "The module could not be removed.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool RemoveSlider(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::size_t a_controlIndex,
        const std::string& a_sliderID,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        auto* slider = yyjson_mut_is_arr(modules) ? yyjson_mut_arr_get(modules, a_controlIndex) : nullptr;
        auto* type = yyjson_mut_is_obj(slider) ? yyjson_mut_obj_get(slider, "type") : nullptr;
        auto* id = yyjson_mut_is_obj(slider) ? yyjson_mut_obj_get(slider, "id") : nullptr;
        if (!yyjson_mut_is_str(type) || !IEquals(yyjson_mut_get_str(type), "slider") ||
            !yyjson_mut_is_str(id) || !IEquals(yyjson_mut_get_str(id), a_sliderID) ||
            !yyjson_mut_arr_remove(modules, a_controlIndex))
        {
            a_error = "The slider being edited no longer exists.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool RenameModule(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::size_t a_controlIndex,
        const std::string& a_name,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        auto* module = yyjson_mut_is_arr(modules) ? yyjson_mut_arr_get(modules, a_controlIndex) : nullptr;
        if (!module)
        {
            a_error = "The selected module is unavailable.";
            return false;
        }
        if (!RenameMutableModule(document.get(), module, a_name, a_error)) return false;
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool UpdateModule(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::size_t a_controlIndex,
        const ModuleUpdate& a_update,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        auto* module = yyjson_mut_is_arr(modules) ? yyjson_mut_arr_get(modules, a_controlIndex) : nullptr;
        auto* kindValue = yyjson_mut_is_obj(module) ? yyjson_mut_obj_get(module, "type") : nullptr;
        if (!yyjson_mut_is_str(kindValue))
        {
            a_error = "The selected module is unavailable.";
            return false;
        }

        const std::string_view kind(yyjson_mut_get_str(kindValue), yyjson_mut_get_len(kindValue));
        const auto name = Trim(a_update.name);
        if (IEquals(kind, "description"))
        {
            const auto description = a_update.text ? Trim(*a_update.text) : std::string{};
            if (description.empty())
            {
                a_error = "Enter a description.";
                return false;
            }
            yyjson_mut_obj_remove_key(module, "displayName");
            yyjson_mut_obj_remove_key(module, "header");
            yyjson_mut_obj_remove_key(module, "text");
            yyjson_mut_obj_remove_key(module, "defaultOpen");
            if ((!name.empty() && !AddString(document.get(), module, "header", name)) ||
                !AddString(document.get(), module, "text", description) ||
                (a_update.defaultOpen.value_or(false) &&
                    !yyjson_mut_obj_add_bool(document.get(), module, "defaultOpen", true)))
            {
                a_error = "The description could not be updated.";
                return false;
            }
        }
        else if (IEquals(kind, "text") || IEquals(kind, "separatorText") ||
                 IEquals(kind, "dropBoxStart"))
        {
            yyjson_mut_obj_remove_key(module, "displayName");
            yyjson_mut_obj_remove_key(module, "label");
            if (!name.empty() && !AddString(document.get(), module, "label", name))
            {
                a_error = "The element text could not be updated.";
                return false;
            }
            if (IEquals(kind, "dropBoxStart"))
            {
                yyjson_mut_obj_remove_key(module, "defaultOpen");
                if (a_update.defaultOpen.value_or(false) &&
                    !yyjson_mut_obj_add_bool(document.get(), module, "defaultOpen", true))
                {
                    a_error = "The drop box state could not be updated.";
                    return false;
                }
            }
        }
        else if (!RenameMutableModule(document.get(), module, name, a_error))
        {
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool MovePage(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const int a_direction,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        const auto count = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_size(pages) : 0;
        const auto destination = static_cast<std::ptrdiff_t>(a_pageIndex) + a_direction;
        if (a_pageIndex >= count || destination < 0 || destination >= static_cast<std::ptrdiff_t>(count))
        {
            a_error = "The page cannot move farther in that direction.";
            return false;
        }
        auto* value = yyjson_mut_arr_remove(pages, a_pageIndex);
        if (!value || !yyjson_mut_arr_insert(pages, value, static_cast<std::size_t>(destination)))
        {
            a_error = "The page order could not be changed.";
            return false;
        }
        if (!SetProfilePageLast(document.get(), root))
        {
            a_error = "The Profile page could not be kept last.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool RenamePage(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::string& a_title,
        std::string& a_error)
    {
        a_error.clear();
        const auto title = Trim(a_title);
        if (title.empty())
        {
            a_error = "Enter a page name.";
            return false;
        }

        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        auto* sourcePages = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "pages") : nullptr;
        if (!yyjson_is_arr(sourcePages) || a_pageIndex >= yyjson_arr_size(sourcePages))
        {
            a_error = "The selected page is unavailable.";
            return false;
        }

        std::size_t index = 0;
        std::size_t maximum = 0;
        yyjson_val* sourcePage = nullptr;
        yyjson_arr_foreach(sourcePages, index, maximum, sourcePage)
        {
            if (index != a_pageIndex)
            {
                if (const auto existing = StringMember(sourcePage, "title"); existing && IEquals(*existing, title))
                {
                    a_error = "A page with that name already exists.";
                    return false;
                }
            }
        }

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        auto* pages = root ? yyjson_mut_obj_get(root, "pages") : nullptr;
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        if (!root || !yyjson_mut_is_obj(page))
        {
            a_error = "The selected page is unavailable.";
            return false;
        }
        yyjson_mut_obj_remove_key(page, "title");
        if (!AddString(document.get(), page, "title", title))
        {
            a_error = "The page could not be renamed.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool SetPageAdvanced(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const bool a_advanced,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        auto* pages = root ? yyjson_mut_obj_get(root, "pages") : nullptr;
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        if (!root || !yyjson_mut_is_obj(page))
        {
            a_error = "The selected page is unavailable.";
            return false;
        }

        yyjson_mut_obj_remove_key(page, "advanced");
        if (a_advanced && !yyjson_mut_obj_add_bool(document.get(), page, "advanced", true))
        {
            a_error = "The page visibility could not be changed.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool RemovePage(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        if (!yyjson_mut_is_arr(pages) || !yyjson_mut_arr_remove(pages, a_pageIndex))
        {
            a_error = "The page could not be removed.";
            return false;
        }
        if (!SetProfilePageLast(document.get(), root))
        {
            a_error = "The Profile page could not be kept last.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool SavePageEdits(
        const std::filesystem::path& a_workingPath,
        const std::filesystem::path& a_savedPath,
        const std::size_t a_workingPageIndex,
        const std::optional<std::size_t> a_savedPageIndex,
        std::size_t& a_resultPageIndex,
        std::string& a_error)
    {
        a_error.clear();
        const auto workingText = ReadText(a_workingPath);
        const auto savedText = ReadText(a_savedPath);
        Document workingDocument(workingText ?
                                     yyjson_read(
                                         const_cast<char*>(workingText->data()),
                                         workingText->size(),
                                         YYJSON_READ_NOFLAG) :
                                     nullptr);
        Document savedDocument(savedText ?
                                   yyjson_read(
                                       const_cast<char*>(savedText->data()),
                                       savedText->size(),
                                       YYJSON_READ_NOFLAG) :
                                   nullptr);
        auto* workingRoot = workingDocument ? yyjson_doc_get_root(workingDocument.get()) : nullptr;
        auto* savedRoot = savedDocument ? yyjson_doc_get_root(savedDocument.get()) : nullptr;
        auto* workingPages = yyjson_is_obj(workingRoot) ? yyjson_obj_get(workingRoot, "pages") : nullptr;
        auto* savedPages = yyjson_is_obj(savedRoot) ? yyjson_obj_get(savedRoot, "pages") : nullptr;
        auto* workingPage = yyjson_is_arr(workingPages) ? yyjson_arr_get(workingPages, a_workingPageIndex) : nullptr;
        if (!yyjson_is_obj(workingPage) || !yyjson_is_arr(savedPages))
        {
            a_error = "The page could not be found in the Dev Mode layout.";
            return false;
        }
        if (a_savedPageIndex && *a_savedPageIndex >= yyjson_arr_size(savedPages))
        {
            a_error = "The saved page could not be found in the menu layout.";
            return false;
        }

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_val_mut_copy(document.get(), savedRoot) : nullptr;
        auto* pages = root ? yyjson_mut_obj_get(root, "pages") : nullptr;
        auto* page = document ? yyjson_val_mut_copy(document.get(), workingPage) : nullptr;
        if (!document || !root || !yyjson_mut_is_arr(pages) || !page)
        {
            a_error = "The selected page could not be prepared for saving.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        if (a_savedPageIndex)
        {
            if (!yyjson_mut_arr_remove(pages, *a_savedPageIndex) ||
                !yyjson_mut_arr_insert(pages, page, *a_savedPageIndex))
            {
                a_error = "The selected page could not replace its saved version.";
                return false;
            }
            a_resultPageIndex = *a_savedPageIndex;
        }
        else
        {
            a_resultPageIndex = yyjson_mut_arr_size(pages);
            if (!yyjson_mut_arr_append(pages, page))
            {
                a_error = "The new page could not be added to the saved menu layout.";
                return false;
            }
        }
        if (!SetProfilePageLast(document.get(), root))
        {
            a_error = "The Profile page could not be kept last.";
            return false;
        }
        return WriteDocument(a_savedPath, document.get(), a_error);
    }

    bool RestorePageEdits(
        const std::filesystem::path& a_savedPath,
        const std::filesystem::path& a_workingPath,
        const std::size_t a_workingPageIndex,
        const std::optional<std::size_t> a_savedPageIndex,
        std::string& a_error)
    {
        a_error.clear();
        if (!a_savedPageIndex)
        {
            return RemovePage(a_workingPath, a_workingPageIndex, a_error);
        }

        const auto savedText = ReadText(a_savedPath);
        const auto workingText = ReadText(a_workingPath);
        Document savedDocument(savedText ?
                                   yyjson_read(
                                       const_cast<char*>(savedText->data()),
                                       savedText->size(),
                                       YYJSON_READ_NOFLAG) :
                                   nullptr);
        Document workingDocument(workingText ?
                                     yyjson_read(
                                         const_cast<char*>(workingText->data()),
                                         workingText->size(),
                                         YYJSON_READ_NOFLAG) :
                                     nullptr);
        auto* savedRoot = savedDocument ? yyjson_doc_get_root(savedDocument.get()) : nullptr;
        auto* workingRoot = workingDocument ? yyjson_doc_get_root(workingDocument.get()) : nullptr;
        auto* savedPages = yyjson_is_obj(savedRoot) ? yyjson_obj_get(savedRoot, "pages") : nullptr;
        auto* workingPages = yyjson_is_obj(workingRoot) ? yyjson_obj_get(workingRoot, "pages") : nullptr;
        auto* savedPage = yyjson_is_arr(savedPages) ? yyjson_arr_get(savedPages, *a_savedPageIndex) : nullptr;
        if (!yyjson_is_obj(savedPage) || !yyjson_is_arr(workingPages) ||
            a_workingPageIndex >= yyjson_arr_size(workingPages))
        {
            a_error = "The selected page could not be found in the saved menu layout.";
            return false;
        }

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_val_mut_copy(document.get(), workingRoot) : nullptr;
        auto* pages = root ? yyjson_mut_obj_get(root, "pages") : nullptr;
        auto* page = document ? yyjson_val_mut_copy(document.get(), savedPage) : nullptr;
        if (!document || !root || !yyjson_mut_is_arr(pages) || !page)
        {
            a_error = "The saved page could not be prepared for restoring.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        if (!yyjson_mut_arr_remove(pages, a_workingPageIndex) ||
            !yyjson_mut_arr_insert(pages, page, a_workingPageIndex))
        {
            a_error = "The selected page could not be restored.";
            return false;
        }
        if (!SetProfilePageLast(document.get(), root))
        {
            a_error = "The Profile page could not be kept last.";
            return false;
        }
        return WriteDocument(a_workingPath, document.get(), a_error);
    }

    bool Save(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::optional<std::size_t> a_controlIndex,
        const Definition& a_definition,
        std::string& a_error,
        const std::optional<std::size_t> a_sourcePageIndex)
    {
        a_error.clear();
        if (!Validate(a_definition, a_error)) return false;

        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot))
        {
            a_error = "The menu file could not be read.";
            return false;
        }
        auto* sourcePages = yyjson_obj_get(sourceRoot, "pages");
        const auto editedPageIndex = a_controlIndex ? a_sourcePageIndex.value_or(a_pageIndex) : a_pageIndex;
        auto* sourcePage = yyjson_is_arr(sourcePages) ? yyjson_arr_get(sourcePages, editedPageIndex) : nullptr;
        auto* sourceModules = yyjson_is_obj(sourcePage) ? yyjson_obj_get(sourcePage, "modules") : nullptr;
        auto* sourceControl = a_controlIndex && yyjson_is_arr(sourceModules) ?
                                  yyjson_arr_get(sourceModules, *a_controlIndex) :
                                  nullptr;

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu file could not be copied for editing.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* pages = yyjson_mut_obj_get(root, "pages");
        auto* page = yyjson_mut_is_arr(pages) ? yyjson_mut_arr_get(pages, a_pageIndex) : nullptr;
        auto* modules = yyjson_mut_is_obj(page) ? yyjson_mut_obj_get(page, "modules") : nullptr;
        if (!yyjson_mut_is_arr(modules))
        {
            a_error = "The selected page is unavailable.";
            return false;
        }
        auto* editedPage = yyjson_mut_arr_get(pages, editedPageIndex);
        auto* editedModules = yyjson_mut_is_obj(editedPage) ? yyjson_mut_obj_get(editedPage, "modules") : nullptr;
        if (a_controlIndex && !yyjson_mut_is_arr(editedModules))
        {
            a_error = "The slider being edited no longer exists.";
            return false;
        }

        std::size_t pagePosition = 0;
        std::size_t pageMaximum = 0;
        yyjson_mut_val* pageCandidate = nullptr;
        yyjson_mut_arr_foreach(pages, pagePosition, pageMaximum, pageCandidate)
        {
            auto* pageModules = yyjson_mut_obj_get(pageCandidate, "modules");
            if (!yyjson_mut_is_arr(pageModules)) continue;
            std::size_t controlPosition = 0;
            std::size_t controlMaximum = 0;
            yyjson_mut_val* control = nullptr;
            yyjson_mut_arr_foreach(pageModules, controlPosition, controlMaximum, control)
            {
                if (a_controlIndex && pagePosition == editedPageIndex && controlPosition == *a_controlIndex) continue;
                auto* kind = yyjson_mut_obj_get(control, "type");
                auto* id = yyjson_mut_obj_get(control, "id");
                if (yyjson_mut_is_str(kind) && yyjson_mut_is_str(id) &&
                    IEquals(yyjson_mut_get_str(kind), "slider") &&
                    IEquals(yyjson_mut_get_str(id), a_definition.id))
                {
                    a_error = "Another slider already uses this name.";
                    return false;
                }
            }
        }

        auto* slider = BuildControl(document.get(), a_definition);
        if (!slider || !CopyUnknownSliderMembers(document.get(), slider, sourceControl))
        {
            a_error = "The slider JSON could not be created.";
            return false;
        }
        if (a_controlIndex)
        {
            auto* existing = yyjson_mut_arr_get(editedModules, *a_controlIndex);
            auto* kind = yyjson_mut_is_obj(existing) ? yyjson_mut_obj_get(existing, "type") : nullptr;
            if (!yyjson_mut_is_str(kind) || !IEquals(yyjson_mut_get_str(kind), "slider"))
            {
                a_error = "The slider being edited no longer exists.";
                return false;
            }
            if (editedPageIndex == a_pageIndex)
            {
                if (!yyjson_mut_arr_replace(modules, *a_controlIndex, slider))
                {
                    a_error = "The slider being edited no longer exists.";
                    return false;
                }
            }
            else if (!yyjson_mut_arr_remove(editedModules, *a_controlIndex) ||
                     !yyjson_mut_arr_append(modules, slider))
            {
                a_error = "The slider could not be moved to the selected page.";
                return false;
            }
        }
        else if (!yyjson_mut_arr_append(modules, slider))
        {
            a_error = "The slider could not be added to the selected page.";
            return false;
        }

        return WriteDocument(a_path, document.get(), a_error);
    }
}  // namespace MPL::SliderCreator
