#include <SliderCreator.h>
#include <SliderSettingCatalog.h>

#include <algorithm>
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

        std::optional<Target> ReadTarget(yyjson_val* a_value, bool& a_structured)
        {
            if (yyjson_is_str(a_value))
            {
                auto setting = Trim(std::string(yyjson_get_str(a_value), yyjson_get_len(a_value)));
                return setting.empty() ? std::nullopt : std::optional<Target>{ { std::move(setting), 1.0, true } };
            }
            if (!yyjson_is_obj(a_value)) return std::nullopt;
            a_structured = true;
            auto setting = StringMember(a_value, "setting");
            if (!setting || (setting = Trim(std::move(*setting)), setting->empty())) return std::nullopt;
            return Target{
                .setting = std::move(*setting),
                .scale = NumberMember(a_value, "scale").value_or(1.0),
                .ignoreLink = BooleanMember(a_value, "ignoreLink"),
            };
        }

        Definition ReadDefinition(yyjson_val* a_control)
        {
            Definition result;
            result.id = StringMember(a_control, "id").value_or("");
            result.label = StringMember(a_control, "label").value_or("");
            result.tooltip = StringMember(a_control, "tooltip").value_or("");
            result.link = StringMember(a_control, "link").value_or("");
            result.localLink = StringMember(a_control, "localLink").value_or("");
            result.hueScales = ReadHueScales(a_control);
            result.invert = BooleanMember(a_control, "invert");
            result.defaultValue = NumberMember(a_control, "default");
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
            result.filtered = structured || result.useTimes || !result.localLink.empty() || result.hueScales ||
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

        bool HasProfileModuleKind(yyjson_val* a_modules, const std::string_view a_kind)
        {
            if (!yyjson_is_arr(a_modules)) return false;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* module = nullptr;
            yyjson_arr_foreach(a_modules, index, maximum, module)
            {
                if (const auto kind = StringMember(module, "kind"); kind && IEquals(*kind, a_kind)) return true;
            }
            return false;
        }

        yyjson_mut_val* EnsureProfileModules(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_root,
            yyjson_val* a_sourceRoot)
        {
            auto* sourceProfilePage = yyjson_is_obj(a_sourceRoot) ? yyjson_obj_get(a_sourceRoot, "profilePage") : nullptr;
            auto* sourceModules = yyjson_is_obj(sourceProfilePage) ? yyjson_obj_get(sourceProfilePage, "modules") : nullptr;
            auto* profilePage = yyjson_mut_obj_get(a_root, "profilePage");
            if (!yyjson_mut_is_obj(profilePage))
            {
                yyjson_mut_obj_remove_key(a_root, "profilePage");
                profilePage = yyjson_mut_obj(a_document);
                if (!profilePage || !yyjson_mut_obj_add_val(a_document, a_root, "profilePage", profilePage)) return nullptr;
            }

            auto* modules = yyjson_mut_obj_get(profilePage, "modules");
            if (!yyjson_mut_is_arr(modules))
            {
                yyjson_mut_obj_remove_key(profilePage, "modules");
                modules = yyjson_mut_arr(a_document);
                if (!modules || !yyjson_mut_obj_add_val(a_document, profilePage, "modules", modules)) return nullptr;
                sourceModules = nullptr;
            }

            for (const auto kind : kRequiredProfileModuleKinds)
            {
                if (HasProfileModuleKind(sourceModules, kind)) continue;
                auto* module = yyjson_mut_obj(a_document);
                if (!module || !AddString(a_document, module, "kind", kind) ||
                    !yyjson_mut_arr_append(modules, module))
                    return nullptr;
            }
            return modules;
        }

        bool IsProfileElementKind(const std::string_view a_kind)
        {
            static constexpr std::array kinds{
                std::string_view("text"),
                std::string_view("separatorText"),
                std::string_view("separator"),
                std::string_view("spacing"),
                std::string_view("boxStart"),
                std::string_view("boxEnd"),
            };
            return std::ranges::any_of(kinds, [&](const auto kind) { return IEquals(a_kind, kind); });
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
            return filter && formIDs && contains &&
                   yyjson_mut_obj_add_val(a_document, filter, "formIDs", formIDs) &&
                   yyjson_mut_obj_add_val(a_document, filter, "contains", contains) &&
                   yyjson_mut_obj_add_val(a_document, a_parent, a_name.data(), filter);
        }

        yyjson_mut_val* BuildControl(yyjson_mut_doc* a_document, const Definition& a_definition)
        {
            auto* control = yyjson_mut_obj(a_document);
            if (!control || !AddString(a_document, control, "kind", "slider") ||
                !AddString(a_document, control, "id", a_definition.id) ||
                !AddString(a_document, control, "label", a_definition.label))
                return nullptr;

            if (!a_definition.tooltip.empty() && !AddString(a_document, control, "tooltip", a_definition.tooltip)) return nullptr;
            if (!a_definition.link.empty() && !AddString(a_document, control, "link", a_definition.link)) return nullptr;
            if (!a_definition.localLink.empty() && !AddString(a_document, control, "localLink", a_definition.localLink)) return nullptr;

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
                        !yyjson_mut_obj_add_real(a_document, setting, "scale", target.scale) ||
                        !yyjson_mut_obj_add_bool(a_document, setting, "ignoreLink", target.ignoreLink) ||
                        !yyjson_mut_arr_append(settings, setting))
                        return nullptr;
                }
                if (!yyjson_mut_obj_add_val(a_document, control, "settings", settings)) return nullptr;
            }

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

            const auto hasWeatherFilter = !a_definition.include.formIDs.empty() || !a_definition.include.contains.empty() ||
                                          !a_definition.exclude.formIDs.empty() || !a_definition.exclude.contains.empty();
            if (hasWeatherFilter)
            {
                auto* filter = yyjson_mut_obj(a_document);
                if (!filter || !AddFilter(a_document, filter, "include", a_definition.include) ||
                    !AddFilter(a_document, filter, "exclude", a_definition.exclude) ||
                    !yyjson_mut_obj_add_val(a_document, control, "weatherFilter", filter))
                    return nullptr;
            }

            if (a_definition.hueScales)
            {
                const auto& scales = *a_definition.hueScales;
                auto* value = yyjson_mut_obj(a_document);
                if (!value || !yyjson_mut_obj_add_real(a_document, value, "red", scales.red) ||
                    !yyjson_mut_obj_add_real(a_document, value, "orange", scales.orange) ||
                    !yyjson_mut_obj_add_real(a_document, value, "yellow", scales.yellow) ||
                    !yyjson_mut_obj_add_real(a_document, value, "green", scales.green) ||
                    !yyjson_mut_obj_add_real(a_document, value, "teal", scales.teal) ||
                    !yyjson_mut_obj_add_real(a_document, value, "blue", scales.blue) ||
                    !yyjson_mut_obj_add_real(a_document, value, "magenta", scales.magenta) ||
                    !yyjson_mut_obj_add_val(a_document, control, "hueScales", value))
                    return nullptr;
            }

            const auto addNumber = [&](const char* a_key, const std::optional<double> a_value)
            {
                return !a_value || yyjson_mut_obj_add_real(a_document, control, a_key, *a_value);
            };
            if (!addNumber("default", a_definition.defaultValue) || !addNumber("min", a_definition.minimum) ||
                !addNumber("max", a_definition.maximum) || !addNumber("step", a_definition.step) ||
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
            if (!a_definition.filtered && std::ranges::any_of(entries, [](const auto* a_entry) { return a_entry->aggregate; }))
            {
                a_error = "All Hues is a creator shortcut; direct sliders must store its seven individual hue bands.";
                return false;
            }
            if (!a_definition.link.empty() &&
                (a_definition.filtered || std::ranges::any_of(entries, [](const auto* a_entry) { return !a_entry->linkable; })))
            {
                a_error = "A direct link requires only linkable, unfiltered settings.";
                return false;
            }
            if (a_definition.filtered)
            {
                std::optional<SliderSettingCatalog::FilterOperation> operation;
                for (const auto* entry : entries)
                {
                    if (!SliderSettingCatalog::IsFilteredOperation(entry->filterOperation))
                    {
                        a_error = "Filtered sliders support only weather brightness, saturation, and hue-shift settings.";
                        return false;
                    }
                    if (operation && operation != entry->filterOperation)
                    {
                        a_error = "Every setting in a filtered slider must use the same operation.";
                        return false;
                    }
                    operation = entry->filterOperation;
                }
                if (!a_definition.localLink.empty() && !std::ranges::any_of(
                        SliderSettingCatalog::Entries(),
                        [&](const auto& a_entry)
                        {
                            return a_entry.domain == SliderSettingCatalog::Domain::weather &&
                                   a_entry.filterOperation == *operation &&
                                   IEquals(a_entry.target, Trim(a_definition.localLink));
                        }))
                {
                    a_error = "The local link must name a target supported by this filtered operation.";
                    return false;
                }
                if (a_definition.hueScales && *operation != SliderSettingCatalog::FilterOperation::saturation)
                {
                    a_error = "Slider-specific hue scales are supported only by filtered saturation sliders.";
                    return false;
                }
            }
            else if (!a_definition.localLink.empty() || a_definition.hueScales)
            {
                a_error = "Local links and slider-specific hue scales require a filtered weather slider.";
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
                    a_error = "Every slider-specific hue scale must be a finite number.";
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
                "kind", "id", "label", "tooltip", "link", "localLink", "hueScales", "setting", "settings",
                "invert", "times", "weatherFilter", "default", "min", "max", "step", "width", "format",
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
                a_error = "The updated menu JSON could not be serialized.";
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
                    a_error = "The temporary menu file could not be written.";
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
            a_error = std::format("The menu file could not be replaced: {}", moveError.message());
            return false;
        }

        bool ReplaceRootStringMember(
            const std::filesystem::path& a_path,
            const std::string_view a_key,
            const std::string_view a_value,
            std::string& a_error,
            const std::string_view a_booleanKey = {},
            const bool a_booleanValue = false)
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
            if (!a_booleanKey.empty())
            {
                yyjson_mut_obj_remove_key(root, a_booleanKey.data());
                auto* boolean = yyjson_mut_bool(document.get(), a_booleanValue);
                if (!boolean || !yyjson_mut_obj_add_val(document.get(), root, a_booleanKey.data(), boolean))
                {
                    a_error = std::format("{} could not be updated.", a_path.filename().string());
                    return false;
                }
            }
            return WriteDocument(a_path, document.get(), a_error);
        }
    }  // namespace

    bool IsRequiredProfileModuleKind(const std::string_view a_kind)
    {
        return std::ranges::any_of(kRequiredProfileModuleKinds, [&](const auto kind) { return IEquals(a_kind, kind); });
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
            Page page{ .title = StringMember(pageValue, "title").value_or(std::format("Page {}", pageIndex + 1)) };
            auto* modules = yyjson_obj_get(pageValue, "modules");
            if (yyjson_is_arr(modules))
            {
                std::size_t controlIndex = 0;
                std::size_t controlMaximum = 0;
                yyjson_val* control = nullptr;
                yyjson_arr_foreach(modules, controlIndex, controlMaximum, control)
                {
                    const auto kind = StringMember(control, "kind");
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
        const std::filesystem::path& a_sourceProfile)
    {
        a_error.clear();
        const auto profileName = Trim(a_profileName);
        if (!ValidProfileName(profileName))
        {
            a_error = "Enter a valid profile name that can be used as a Windows folder name.";
            return false;
        }

        const auto copying = !a_sourceProfile.empty();
        const auto menuSource = copying ? a_sourceProfile / "skseMenu.json" :
                                          a_tuningRoot / "newSkseMenu.json";
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
                          "Luma/Tuning/newSkseMenu.json is missing or does not contain a valid pages array.";
            return false;
        }
        if (copying)
        {
            std::error_code sourceError;
            if (!std::filesystem::is_directory(a_sourceProfile, sourceError) || sourceError)
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
        }

        const auto profileDirectory = a_tuningRoot / profileName;
        std::error_code filesystemError;
        if (std::filesystem::exists(profileDirectory, filesystemError))
        {
            a_error = "A profile folder with that name already exists.";
            return false;
        }
        if (filesystemError || !std::filesystem::create_directory(profileDirectory, filesystemError))
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
                const auto relative = std::filesystem::relative(iterator->path(), a_sourceProfile, filesystemError);
                if (filesystemError) break;
                const auto destination = profileDirectory / relative;
                if (iterator->is_directory(filesystemError))
                {
                    std::filesystem::create_directories(destination, filesystemError);
                }
                else if (iterator->is_regular_file(filesystemError))
                {
                    std::filesystem::create_directories(destination.parent_path(), filesystemError);
                    if (!filesystemError)
                    {
                        std::filesystem::copy_file(
                            iterator->path(),
                            destination,
                            std::filesystem::copy_options::overwrite_existing,
                            filesystemError);
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
                    a_error,
                    "lockEditMode",
                    false))
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
        {
            std::ofstream settingsFile(profileDirectory / "profileSettings.json", std::ios::binary | std::ios::trunc);
            settingsFile << std::format(
                "{{\n  \"profile\": \"{}\",\n  \"EnableProfile\": true,\n  \"ShowAdvanced\": false,\n  \"profilePriority\": 0\n}}\n",
                profileName);
            if (!settingsFile)
            {
                settingsFile.close();
                removeIncompleteProfile();
                a_error = "The new profile's profileSettings.json could not be written.";
                return false;
            }
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
        auto* sourceProfilePage = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "profilePage") : nullptr;
        const auto sourceProfileOrder = NumberMember(sourceProfilePage, "order");
        if (sourceProfileOrder && *sourceProfileOrder >= static_cast<double>(pageIndex))
        {
            auto* profilePage = yyjson_mut_obj_get(root, "profilePage");
            yyjson_mut_obj_remove_key(profilePage, "order");
            auto* order = yyjson_mut_uint(document.get(), static_cast<std::uint64_t>(pageIndex + 1));
            if (!order || !yyjson_mut_obj_add_val(document.get(), profilePage, "order", order))
            {
                a_error = "The Profile page order could not be preserved.";
                return std::nullopt;
            }
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
        const bool a_advanced,
        std::string& a_error)
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
        const auto labelKey = kind == "text" || kind == "separatorText" || kind == "boxStart" ?
                                  "label" : "header";
        if (!yyjson_mut_is_arr(modules) || !module || !AddString(document.get(), module, "kind", kind) ||
            (!a_label.empty() && !AddString(document.get(), module, labelKey, a_label)) ||
            (!a_setting.empty() && !AddString(document.get(), module, "setting", a_setting)) ||
            (a_advanced && !yyjson_mut_obj_add_bool(document.get(), module, "advanced", true)) ||
            !yyjson_mut_arr_append(modules, module))
        {
            a_error = "The module could not be added to this page.";
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

    bool AddProfileElement(
        const std::filesystem::path& a_path,
        const std::string& a_kind,
        const std::string& a_label,
        std::string& a_error)
    {
        a_error.clear();
        const auto kind = Trim(a_kind);
        if (!IsProfileElementKind(kind))
        {
            a_error = "Select a profile page element to add.";
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
        auto* modules = EnsureProfileModules(document.get(), root, sourceRoot);
        auto* module = yyjson_mut_obj(document.get());
        const auto labelKey = kind == "text" || kind == "separatorText" || kind == "boxStart" ?
                                  "label" : "";
        if (!modules || !module || !AddString(document.get(), module, "kind", kind) ||
            (!a_label.empty() && !AddString(document.get(), module, labelKey, a_label)) ||
            !yyjson_mut_arr_append(modules, module))
        {
            a_error = "The element could not be added to the Profile page.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool MoveProfileModule(
        const std::filesystem::path& a_path,
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
        auto* modules = EnsureProfileModules(document.get(), root, sourceRoot);
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
            a_error = "The Profile page module order could not be changed.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool RemoveProfileModule(
        const std::filesystem::path& a_path,
        const std::size_t a_controlIndex,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        auto* sourceProfilePage = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "profilePage") : nullptr;
        auto* sourceModules = yyjson_is_obj(sourceProfilePage) ? yyjson_obj_get(sourceProfilePage, "modules") : nullptr;
        const auto sourceCount = yyjson_is_arr(sourceModules) ? yyjson_arr_size(sourceModules) : 0;
        if (a_controlIndex >= sourceCount)
        {
            a_error = "Required Profile page modules cannot be removed.";
            return false;
        }
        auto* sourceModule = yyjson_arr_get(sourceModules, a_controlIndex);
        const auto sourceKind = StringMember(sourceModule, "kind").value_or("");
        if (IsRequiredProfileModuleKind(sourceKind))
        {
            a_error = "Required Profile page modules cannot be removed.";
            return false;
        }

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        auto* modules = EnsureProfileModules(document.get(), root, sourceRoot);
        if (!yyjson_mut_is_arr(modules) || !yyjson_mut_arr_remove(modules, a_controlIndex))
        {
            a_error = "The Profile page element could not be removed.";
            return false;
        }
        return WriteDocument(a_path, document.get(), a_error);
    }

    bool MoveProfilePage(
        const std::filesystem::path& a_path,
        const int a_direction,
        std::string& a_error)
    {
        a_error.clear();
        const auto text = ReadText(a_path);
        Document source(text ? yyjson_read(const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG) : nullptr);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        auto* sourceProfilePage = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "profilePage") : nullptr;
        auto* sourcePages = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "pages") : nullptr;
        const auto pageCount = yyjson_is_arr(sourcePages) ? yyjson_arr_size(sourcePages) : 0;
        const auto currentOrder = static_cast<std::ptrdiff_t>(
            std::clamp(
                NumberMember(sourceProfilePage, "order").value_or(0.0),
                0.0,
                static_cast<double>(pageCount)));
        const auto destination = currentOrder + a_direction;
        if (destination < 0 || destination > static_cast<std::ptrdiff_t>(pageCount))
        {
            a_error = "The Profile page cannot move farther in that direction.";
            return false;
        }

        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document && yyjson_is_obj(sourceRoot) ? yyjson_val_mut_copy(document.get(), sourceRoot) : nullptr;
        if (!root)
        {
            a_error = "The menu layout could not be read.";
            return false;
        }
        yyjson_mut_doc_set_root(document.get(), root);
        if (!EnsureProfileModules(document.get(), root, sourceRoot))
        {
            a_error = "The Profile page could not be prepared for editing.";
            return false;
        }
        auto* profilePage = yyjson_mut_obj_get(root, "profilePage");
        yyjson_mut_obj_remove_key(profilePage, "order");
        auto* order = yyjson_mut_uint(document.get(), static_cast<std::uint64_t>(destination));
        if (!order || !yyjson_mut_obj_add_val(document.get(), profilePage, "order", order))
        {
            a_error = "The Profile page order could not be changed.";
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
        auto* sourceProfilePage = yyjson_is_obj(sourceRoot) ? yyjson_obj_get(sourceRoot, "profilePage") : nullptr;
        const auto sourcePageCount = yyjson_is_arr(yyjson_obj_get(sourceRoot, "pages")) ?
                                         yyjson_arr_size(yyjson_obj_get(sourceRoot, "pages")) :
                                         0;
        const auto profileOrder = static_cast<std::size_t>(
            std::clamp(
                NumberMember(sourceProfilePage, "order").value_or(0.0),
                0.0,
                static_cast<double>(sourcePageCount)));
        if (a_pageIndex < profileOrder)
        {
            auto* profilePage = yyjson_mut_obj_get(root, "profilePage");
            yyjson_mut_obj_remove_key(profilePage, "order");
            auto* order = yyjson_mut_uint(document.get(), static_cast<std::uint64_t>(profileOrder - 1));
            if (!order || !yyjson_mut_obj_add_val(document.get(), profilePage, "order", order))
            {
                a_error = "The Profile page order could not be preserved.";
                return false;
            }
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
            a_error = "The page could not be found in the Edit Mode layout.";
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
        return WriteDocument(a_workingPath, document.get(), a_error);
    }

    bool Save(
        const std::filesystem::path& a_path,
        const std::size_t a_pageIndex,
        const std::optional<std::size_t> a_controlIndex,
        const Definition& a_definition,
        std::string& a_error)
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
        auto* sourcePage = yyjson_is_arr(sourcePages) ? yyjson_arr_get(sourcePages, a_pageIndex) : nullptr;
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
                if (a_controlIndex && pagePosition == a_pageIndex && controlPosition == *a_controlIndex) continue;
                auto* kind = yyjson_mut_obj_get(control, "kind");
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
            auto* existing = yyjson_mut_arr_get(modules, *a_controlIndex);
            auto* kind = yyjson_mut_is_obj(existing) ? yyjson_mut_obj_get(existing, "kind") : nullptr;
            if (!yyjson_mut_is_str(kind) || !IEquals(yyjson_mut_get_str(kind), "slider") ||
                !yyjson_mut_arr_replace(modules, *a_controlIndex, slider))
            {
                a_error = "The slider being edited no longer exists.";
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
