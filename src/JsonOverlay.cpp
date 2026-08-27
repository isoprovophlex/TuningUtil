#include <JsonOverlay.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <unordered_set>
#include <yyjson.h>

namespace MPL::JsonOverlay
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
        constexpr std::array kRecordFilterKeys{
            std::string_view{ "weatherInclusions" },
            std::string_view{ "weatherExclusions" },
            std::string_view{ "effectPointLightInclusions" },
            std::string_view{ "effectPointLightExclusions" },
            std::string_view{ "lightingTemplateInclusions" },
            std::string_view{ "lightingTemplateExclusions" },
        };
        constexpr std::array kPluginFilterKeys{
            std::string_view{ "pluginInclusions" },
            std::string_view{ "pluginExclusions" },
            std::string_view{ "effectLightingPluginInclusions" },
            std::string_view{ "effectLightingPluginExclusions" },
            std::string_view{ "lightingTemplatePluginInclusions" },
            std::string_view{ "lightingTemplatePluginExclusions" },
        };

        bool KeyEquals(yyjson_val* a_key, const std::string_view a_expected)
        {
            return yyjson_is_str(a_key) && yyjson_get_len(a_key) == a_expected.size() &&
                   std::memcmp(yyjson_get_str(a_key), a_expected.data(), a_expected.size()) == 0;
        }

        long double NumericValue(yyjson_val* a_value)
        {
            if (yyjson_is_sint(a_value)) return static_cast<long double>(yyjson_get_sint(a_value));
            if (yyjson_is_uint(a_value)) return static_cast<long double>(yyjson_get_uint(a_value));
            return static_cast<long double>(yyjson_get_real(a_value));
        }

        bool ValuesEquivalent(yyjson_val* a_left, yyjson_val* a_right)
        {
            if (yyjson_equals(a_left, a_right)) return true;
            if (yyjson_is_num(a_left) && yyjson_is_num(a_right))
            {
                return NumericValue(a_left) == NumericValue(a_right);
            }
            if (yyjson_is_arr(a_left) && yyjson_is_arr(a_right))
            {
                const auto size = yyjson_arr_size(a_left);
                if (size != yyjson_arr_size(a_right)) return false;
                for (std::size_t index = 0; index < size; ++index)
                {
                    if (!ValuesEquivalent(yyjson_arr_get(a_left, index), yyjson_arr_get(a_right, index))) return false;
                }
                return true;
            }
            if (yyjson_is_obj(a_left) && yyjson_is_obj(a_right))
            {
                if (yyjson_obj_size(a_left) != yyjson_obj_size(a_right)) return false;
                yyjson_obj_iter iterator = yyjson_obj_iter_with(a_left);
                while (auto* key = yyjson_obj_iter_next(&iterator))
                {
                    auto* rightValue = yyjson_obj_getn(a_right, yyjson_get_str(key), yyjson_get_len(key));
                    if (!rightValue || !ValuesEquivalent(yyjson_obj_iter_get_val(key), rightValue)) return false;
                }
                return true;
            }
            return false;
        }

        std::string_view FilterExactKey(yyjson_val* a_key)
        {
            if (std::ranges::any_of(kRecordFilterKeys, [&](const auto a_expected)
                    { return KeyEquals(a_key, a_expected); }))
                return "formIDs";
            if (std::ranges::any_of(kPluginFilterKeys, [&](const auto a_expected)
                    { return KeyEquals(a_key, a_expected); }))
                return "exact";
            return {};
        }

        std::string NormalizedString(yyjson_val* a_value)
        {
            if (!yyjson_is_str(a_value))
            {
                return {};
            }
            std::string result(yyjson_get_str(a_value), yyjson_get_len(a_value));
            std::ranges::transform(
                result,
                result.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return result;
        }

        bool AppendUniqueStrings(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_result,
            yyjson_val* a_source,
            std::unordered_set<std::string>& a_seen)
        {
            if (!yyjson_is_arr(a_source))
            {
                return true;
            }
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* value = nullptr;
            yyjson_arr_foreach(a_source, index, maximum, value)
            {
                if (yyjson_is_str(value) && !a_seen.insert(NormalizedString(value)).second)
                {
                    continue;
                }
                auto* copy = yyjson_val_mut_copy(a_document, value);
                if (!copy || !yyjson_mut_arr_append(a_result, copy))
                {
                    return false;
                }
            }
            return true;
        }

        yyjson_val* FilterArray(
            yyjson_val* a_value,
            const std::string_view a_key,
            const std::string_view a_exactKey)
        {
            if (yyjson_is_arr(a_value))
            {
                return a_key == a_exactKey ? a_value : nullptr;
            }
            return yyjson_is_obj(a_value) ? yyjson_obj_getn(a_value, a_key.data(), a_key.size()) : nullptr;
        }

        yyjson_mut_val* CopyFilter(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value,
            const std::string_view a_exactKey)
        {
            if (!yyjson_is_arr(a_value) && !yyjson_is_obj(a_value))
            {
                return yyjson_val_mut_copy(a_document, a_value);
            }

            auto* result = yyjson_mut_obj(a_document);
            const std::array keys{ a_exactKey, std::string_view{ "contains" } };
            for (const auto key : keys)
            {
                auto* values = yyjson_mut_arr(a_document);
                std::unordered_set<std::string> seen;
                auto* member = yyjson_mut_strncpy(a_document, key.data(), key.size());
                if (!AppendUniqueStrings(a_document, values, FilterArray(a_value, key, a_exactKey), seen) ||
                    !member || !yyjson_mut_obj_add(result, member, values))
                {
                    return nullptr;
                }
            }
            return result;
        }

        yyjson_mut_val* MergeFilters(
            yyjson_mut_doc* a_document,
            yyjson_val* a_defaults,
            yyjson_val* a_overrides,
            const std::string_view a_exactKey)
        {
            if (yyjson_is_arr(a_defaults))
            {
                auto* result = yyjson_mut_arr(a_document);
                std::unordered_set<std::string> seen;
                return AppendUniqueStrings(a_document, result, a_defaults, seen) &&
                               AppendUniqueStrings(
                                   a_document,
                                   result,
                                   FilterArray(a_overrides, a_exactKey, a_exactKey),
                                   seen) ?
                           result :
                           nullptr;
            }

            auto* result = yyjson_mut_obj(a_document);
            const std::array keys{ a_exactKey, std::string_view{ "contains" } };
            for (const auto key : keys)
            {
                auto* values = yyjson_mut_arr(a_document);
                std::unordered_set<std::string> seen;
                auto* member = yyjson_mut_strncpy(a_document, key.data(), key.size());
                if (!AppendUniqueStrings(a_document, values, FilterArray(a_defaults, key, a_exactKey), seen) ||
                    !AppendUniqueStrings(a_document, values, FilterArray(a_overrides, key, a_exactKey), seen) ||
                    !member || !yyjson_mut_obj_add(result, member, values))
                {
                    return nullptr;
                }
            }
            return result;
        }

        yyjson_mut_val* DifferenceStringList(
            yyjson_mut_doc* a_document,
            yyjson_val* a_current,
            yyjson_val* a_defaults)
        {
            std::unordered_set<std::string> defaultValues;
            std::size_t index = 0;
            std::size_t maximum = 0;
            yyjson_val* value = nullptr;
            if (yyjson_is_arr(a_defaults))
            {
                yyjson_arr_foreach(a_defaults, index, maximum, value)
                {
                    if (yyjson_is_str(value)) defaultValues.insert(NormalizedString(value));
                }
            }

            auto* result = yyjson_mut_arr(a_document);
            std::unordered_set<std::string> additions;
            if (yyjson_is_arr(a_current))
            {
                yyjson_arr_foreach(a_current, index, maximum, value)
                {
                    const auto normalized = NormalizedString(value);
                    if (yyjson_is_str(value) &&
                        (defaultValues.contains(normalized) || !additions.insert(normalized).second))
                    {
                        continue;
                    }
                    auto* copy = yyjson_val_mut_copy(a_document, value);
                    if (!copy || !yyjson_mut_arr_append(result, copy)) return nullptr;
                }
            }
            return result;
        }

        yyjson_mut_val* DifferenceFilters(
            yyjson_mut_doc* a_document,
            yyjson_val* a_current,
            yyjson_val* a_defaults,
            const std::string_view a_exactKey)
        {
            if (yyjson_is_arr(a_current))
            {
                auto* result = DifferenceStringList(
                    a_document,
                    a_current,
                    FilterArray(a_defaults, a_exactKey, a_exactKey));
                return result && yyjson_mut_arr_size(result) != 0 ? result : nullptr;
            }

            auto* result = yyjson_mut_obj(a_document);
            const std::array keys{ a_exactKey, std::string_view{ "contains" } };
            for (const auto key : keys)
            {
                auto* difference = DifferenceStringList(
                    a_document,
                    FilterArray(a_current, key, a_exactKey),
                    FilterArray(a_defaults, key, a_exactKey));
                if (!difference)
                {
                    return nullptr;
                }
                auto* member = yyjson_mut_strncpy(a_document, key.data(), key.size());
                if (yyjson_mut_arr_size(difference) != 0 &&
                    (!member || !yyjson_mut_obj_add(result, member, difference)))
                {
                    return nullptr;
                }
            }
            return yyjson_mut_obj_size(result) == 0 ? nullptr : result;
        }

        Document Parse(const std::string_view a_json, const std::string_view a_name, std::string& a_error)
        {
            yyjson_read_err error{};
            auto* document = yyjson_read_opts(
                const_cast<char*>(a_json.data()),
                a_json.size(),
                YYJSON_READ_NOFLAG,
                nullptr,
                &error);
            if (!document)
            {
                a_error = std::format(
                    "Could not parse {} at byte {}: {}",
                    a_name,
                    error.pos,
                    error.msg ? error.msg : "unknown JSON error");
            }
            return Document(document);
        }

        bool AddMember(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_object,
            yyjson_val* a_key,
            yyjson_mut_val* a_value)
        {
            auto* key = yyjson_mut_strncpy(a_document, yyjson_get_str(a_key), yyjson_get_len(a_key));
            return key && a_value && yyjson_mut_obj_add(a_object, key, a_value);
        }

        yyjson_mut_val* CopyOverrideValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value)
        {
            return yyjson_val_mut_copy(a_document, a_value);
        }

        yyjson_mut_val* MergeValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_defaults,
            yyjson_val* a_overrides,
            const bool a_userSettingsRules)
        {
            if (!a_overrides)
            {
                return yyjson_val_mut_copy(a_document, a_defaults);
            }
            if (!a_defaults || !yyjson_is_obj(a_defaults) || !yyjson_is_obj(a_overrides))
            {
                return CopyOverrideValue(a_document, a_overrides);
            }

            auto* result = yyjson_mut_obj(a_document);
            yyjson_obj_iter defaultIterator = yyjson_obj_iter_with(a_defaults);
            while (auto* key = yyjson_obj_iter_next(&defaultIterator))
            {
                auto* defaultValue = yyjson_obj_iter_get_val(key);
                auto* overrideValue = yyjson_obj_getn(a_overrides, yyjson_get_str(key), yyjson_get_len(key));
                const auto exactKey = FilterExactKey(key);
                auto* mergedValue = a_userSettingsRules && !exactKey.empty() && overrideValue ?
                                        MergeFilters(a_document, defaultValue, overrideValue, exactKey) :
                                    (!a_userSettingsRules && !exactKey.empty() &&
                                             yyjson_is_arr(defaultValue) && yyjson_is_obj(overrideValue) ?
                                        MergeFilters(a_document, defaultValue, overrideValue, exactKey) :
                                        (!a_userSettingsRules && !exactKey.empty() &&
                                                 yyjson_is_obj(defaultValue) && yyjson_is_arr(overrideValue) ?
                                                CopyFilter(a_document, overrideValue, exactKey) :
                                                MergeValue(a_document, defaultValue, overrideValue, a_userSettingsRules)));
                if (!AddMember(a_document, result, key, mergedValue))
                {
                    return nullptr;
                }
            }

            yyjson_obj_iter overrideIterator = yyjson_obj_iter_with(a_overrides);
            while (auto* key = yyjson_obj_iter_next(&overrideIterator))
            {
                if (yyjson_obj_getn(a_defaults, yyjson_get_str(key), yyjson_get_len(key)))
                {
                    continue;
                }
                if (!AddMember(
                        a_document,
                        result,
                        key,
                        CopyOverrideValue(a_document, yyjson_obj_iter_get_val(key))))
                {
                    return nullptr;
                }
            }
            return result;
        }

        yyjson_mut_val* DifferenceValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_current,
            yyjson_val* a_defaults)
        {
            if (a_defaults && ValuesEquivalent(a_current, a_defaults))
            {
                return nullptr;
            }
            if (!a_defaults && yyjson_is_obj(a_current))
            {
                auto* result = yyjson_mut_obj(a_document);
                yyjson_obj_iter iterator = yyjson_obj_iter_with(a_current);
                while (auto* key = yyjson_obj_iter_next(&iterator))
                {
                    auto* difference = DifferenceValue(a_document, yyjson_obj_iter_get_val(key), nullptr);
                    if (difference && !AddMember(a_document, result, key, difference))
                    {
                        return nullptr;
                    }
                }
                return yyjson_mut_obj_size(result) == 0 ? nullptr : result;
            }
            if (!a_defaults || !yyjson_is_obj(a_current) || !yyjson_is_obj(a_defaults))
            {
                return yyjson_val_mut_copy(a_document, a_current);
            }

            auto* result = yyjson_mut_obj(a_document);
            yyjson_obj_iter currentIterator = yyjson_obj_iter_with(a_current);
            while (auto* key = yyjson_obj_iter_next(&currentIterator))
            {
                auto* currentValue = yyjson_obj_iter_get_val(key);
                auto* defaultValue = yyjson_obj_getn(a_defaults, yyjson_get_str(key), yyjson_get_len(key));
                const auto exactKey = FilterExactKey(key);
                auto* difference = !exactKey.empty() && defaultValue ?
                                       DifferenceFilters(a_document, currentValue, defaultValue, exactKey) :
                                       DifferenceValue(a_document, currentValue, defaultValue);
                if (difference)
                {
                    if (!AddMember(a_document, result, key, difference))
                    {
                        return nullptr;
                    }
                }
            }
            return yyjson_mut_obj_size(result) == 0 ? nullptr : result;
        }

        yyjson_mut_val* ProjectLikeValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_source,
            yyjson_val* a_schema)
        {
            if (!yyjson_is_obj(a_source) || !yyjson_is_obj(a_schema))
            {
                return yyjson_val_mut_copy(a_document, a_source);
            }

            auto* result = yyjson_mut_obj(a_document);
            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_schema);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                auto* source = yyjson_obj_getn(a_source, yyjson_get_str(key), yyjson_get_len(key));
                if (source && !AddMember(
                                  a_document,
                                  result,
                                  key,
                                  ProjectLikeValue(a_document, source, yyjson_obj_iter_get_val(key))))
                {
                    return nullptr;
                }
            }
            return result;
        }

        yyjson_val* FindPath(yyjson_val* a_root, const std::string_view a_path)
        {
            auto* value = a_root;
            std::size_t start = 0;
            while (value && start < a_path.size())
            {
                const auto separator = a_path.find('.', start);
                const auto length = (separator == std::string_view::npos ? a_path.size() : separator) - start;
                value = yyjson_is_obj(value) ? yyjson_obj_getn(value, a_path.data() + start, length) : nullptr;
                if (separator == std::string_view::npos)
                {
                    break;
                }
                start = separator + 1;
            }
            return value;
        }

        bool PutPath(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_root,
            const std::string_view a_path,
            yyjson_val* a_value)
        {
            auto* object = a_root;
            std::size_t start = 0;
            while (start < a_path.size())
            {
                const auto separator = a_path.find('.', start);
                const auto end = separator == std::string_view::npos ? a_path.size() : separator;
                const auto length = end - start;
                if (separator == std::string_view::npos)
                {
                    auto* key = yyjson_mut_strncpy(a_document, a_path.data() + start, length);
                    auto* value = yyjson_val_mut_copy(a_document, a_value);
                    return key && value && yyjson_mut_obj_put(object, key, value);
                }

                auto* child = yyjson_mut_obj_getn(object, a_path.data() + start, length);
                if (!child)
                {
                    auto* key = yyjson_mut_strncpy(a_document, a_path.data() + start, length);
                    child = yyjson_mut_obj(a_document);
                    if (!key || !child || !yyjson_mut_obj_add(object, key, child))
                    {
                        return false;
                    }
                }
                if (!yyjson_mut_is_obj(child))
                {
                    return false;
                }
                object = child;
                start = separator + 1;
            }
            return false;
        }

        bool RemovePath(yyjson_mut_val* a_root, const std::string_view a_path)
        {
            auto* object = a_root;
            std::size_t start = 0;
            while (yyjson_mut_is_obj(object) && start < a_path.size())
            {
                const auto separator = a_path.find('.', start);
                const auto end = separator == std::string_view::npos ? a_path.size() : separator;
                const auto length = end - start;
                if (separator == std::string_view::npos)
                {
                    yyjson_mut_obj_remove_keyn(object, a_path.data() + start, length);
                    return true;
                }
                object = yyjson_mut_obj_getn(object, a_path.data() + start, length);
                start = separator + 1;
            }
            return true;
        }

        void RemoveLikeValue(yyjson_mut_val* a_source, yyjson_val* a_schema)
        {
            if (!yyjson_mut_is_obj(a_source) || !yyjson_is_obj(a_schema))
            {
                return;
            }

            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_schema);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                const auto* name = yyjson_get_str(key);
                const auto length = yyjson_get_len(key);
                auto* sourceValue = yyjson_mut_obj_getn(a_source, name, length);
                if (!sourceValue)
                {
                    continue;
                }

                auto* schemaValue = yyjson_obj_iter_get_val(key);
                if (yyjson_is_obj(schemaValue) && yyjson_obj_size(schemaValue) != 0 && yyjson_mut_is_obj(sourceValue))
                {
                    RemoveLikeValue(sourceValue, schemaValue);
                    if (yyjson_mut_obj_size(sourceValue) == 0)
                    {
                        yyjson_mut_obj_remove_keyn(a_source, name, length);
                    }
                }
                else if (!yyjson_is_obj(schemaValue) || yyjson_obj_size(schemaValue) != 0)
                {
                    yyjson_mut_obj_remove_keyn(a_source, name, length);
                }
            }
        }

        std::optional<std::string> Write(yyjson_mut_doc* a_document, std::string& a_error)
        {
            std::size_t length = 0;
            yyjson_write_err error{};
            auto* data = yyjson_mut_write_opts(
                a_document,
                YYJSON_WRITE_PRETTY_TWO_SPACES,
                nullptr,
                &length,
                &error);
            if (!data)
            {
                a_error = error.msg ? error.msg : "Could not serialize JSON";
                return std::nullopt;
            }

            std::string result(data, length);
            std::free(data);
            return result;
        }

        std::optional<std::string> Combine(
            const std::string_view a_base,
            const std::string_view a_changes,
            const bool a_userSettingsRules,
            std::string& a_error)
        {
            a_error.clear();
            auto base = Parse(a_base, a_userSettingsRules ? "default settings" : "current settings", a_error);
            auto changes = base ?
                               Parse(a_changes, a_userSettingsRules ? "user settings" : "preset settings", a_error) :
                               nullptr;
            auto* baseRoot = base ? yyjson_doc_get_root(base.get()) : nullptr;
            auto* changeRoot = changes ? yyjson_doc_get_root(changes.get()) : nullptr;
            if (!yyjson_is_obj(baseRoot) || !yyjson_is_obj(changeRoot))
            {
                if (a_error.empty()) a_error = "Both JSON documents must contain an object at the root";
                return std::nullopt;
            }

            MutableDocument result(yyjson_mut_doc_new(nullptr));
            auto* root = MergeValue(result.get(), baseRoot, changeRoot, a_userSettingsRules);
            if (!root)
            {
                a_error = a_userSettingsRules ?
                              "Could not merge default and user settings" :
                              "Could not overlay preset settings";
                return std::nullopt;
            }
            yyjson_mut_doc_set_root(result.get(), root);
            return Write(result.get(), a_error);
        }
    }  // namespace

    std::optional<bool> Equivalent(
        const std::string_view a_left,
        const std::string_view a_right,
        std::string& a_error)
    {
        a_error.clear();
        const auto left = Parse(a_left, "left JSON document", a_error);
        const auto right = left ? Parse(a_right, "right JSON document", a_error) : nullptr;
        if (!left || !right)
        {
            return std::nullopt;
        }
        return ValuesEquivalent(yyjson_doc_get_root(left.get()), yyjson_doc_get_root(right.get()));
    }

    std::optional<bool> BooleanMember(
        const std::string_view a_json,
        const std::string_view a_name)
    {
        std::string error;
        const auto document = Parse(a_json, "JSON object", error);
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        auto* value = yyjson_is_obj(root) ? yyjson_obj_getn(root, a_name.data(), a_name.size()) : nullptr;
        return yyjson_is_bool(value) ? std::optional<bool>{ yyjson_get_bool(value) } : std::nullopt;
    }

    std::optional<std::string> SetBooleanMember(
        const std::string_view a_json,
        const std::string_view a_name,
        const bool a_value,
        std::string& a_error)
    {
        a_error.clear();
        const auto source = Parse(a_json, "JSON object", a_error);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot))
        {
            if (a_error.empty()) a_error = "The JSON document must contain an object at the root";
            return std::nullopt;
        }

        MutableDocument result(yyjson_mut_doc_new(nullptr));
        auto* root = yyjson_val_mut_copy(result.get(), sourceRoot);
        auto* key = yyjson_mut_strncpy(result.get(), a_name.data(), a_name.size());
        auto* value = yyjson_mut_bool(result.get(), a_value);
        if (!root || !key || !value || !yyjson_mut_obj_put(root, key, value))
        {
            a_error = "Could not update the JSON boolean value";
            return std::nullopt;
        }
        yyjson_mut_doc_set_root(result.get(), root);
        return Write(result.get(), a_error);
    }

    std::optional<std::string> Merge(
        const std::string_view a_defaults,
        const std::string_view a_overrides,
        std::string& a_error)
    {
        return Combine(a_defaults, a_overrides, true, a_error);
    }

    std::optional<std::string> Overlay(
        const std::string_view a_current,
        const std::string_view a_changes,
        std::string& a_error)
    {
        return Combine(a_current, a_changes, false, a_error);
    }

    std::optional<std::string> Difference(
        const std::string_view a_current,
        const std::string_view a_defaults,
        std::string& a_error)
    {
        a_error.clear();
        auto current = Parse(a_current, "current settings", a_error);
        if (!current)
        {
            return std::nullopt;
        }
        auto defaults = Parse(a_defaults, "default settings", a_error);
        if (!defaults)
        {
            return std::nullopt;
        }

        auto* currentRoot = yyjson_doc_get_root(current.get());
        auto* defaultRoot = yyjson_doc_get_root(defaults.get());
        if (!yyjson_is_obj(currentRoot) || !yyjson_is_obj(defaultRoot))
        {
            a_error = "Current and default settings must both contain a JSON object at the root";
            return std::nullopt;
        }

        MutableDocument result(yyjson_mut_doc_new(nullptr));
        auto* root = DifferenceValue(result.get(), currentRoot, defaultRoot);
        if (!root)
        {
            root = yyjson_mut_obj(result.get());
        }
        yyjson_mut_doc_set_root(result.get(), root);
        return Write(result.get(), a_error);
    }

    std::optional<std::string> ProjectLike(
        const std::string_view a_source,
        const std::string_view a_schema,
        std::string& a_error)
    {
        a_error.clear();
        auto source = Parse(a_source, "projection source", a_error);
        auto schema = source ? Parse(a_schema, "projection schema", a_error) : nullptr;
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        auto* schemaRoot = schema ? yyjson_doc_get_root(schema.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot) || !yyjson_is_obj(schemaRoot))
        {
            if (a_error.empty()) a_error = "Projection source and schema must contain JSON objects at the root";
            return std::nullopt;
        }

        MutableDocument result(yyjson_mut_doc_new(nullptr));
        auto* root = ProjectLikeValue(result.get(), sourceRoot, schemaRoot);
        if (!root)
        {
            a_error = "Could not project settings from the supplied schema";
            return std::nullopt;
        }
        yyjson_mut_doc_set_root(result.get(), root);
        return Write(result.get(), a_error);
    }

    std::optional<std::string> ProjectPaths(
        const std::string_view a_source,
        const std::span<const std::string> a_paths,
        std::string& a_error)
    {
        a_error.clear();
        auto source = Parse(a_source, "projection source", a_error);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot))
        {
            if (a_error.empty()) a_error = "Projection source must contain a JSON object at the root";
            return std::nullopt;
        }

        MutableDocument result(yyjson_mut_doc_new(nullptr));
        auto* root = yyjson_mut_obj(result.get());
        for (const auto& path : a_paths)
        {
            if (auto* value = FindPath(sourceRoot, path); value && !PutPath(result.get(), root, path, value))
            {
                a_error = std::format("Could not project setting path {}", path);
                return std::nullopt;
            }
        }
        yyjson_mut_doc_set_root(result.get(), root);
        return Write(result.get(), a_error);
    }

    std::optional<std::string> RemovePaths(
        const std::string_view a_source,
        const std::span<const std::string> a_paths,
        std::string& a_error)
    {
        a_error.clear();
        auto source = Parse(a_source, "path-removal source", a_error);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot))
        {
            if (a_error.empty()) a_error = "Path-removal source must contain a JSON object at the root";
            return std::nullopt;
        }

        MutableDocument result(yyjson_mut_doc_new(nullptr));
        auto* root = yyjson_val_mut_copy(result.get(), sourceRoot);
        if (!root)
        {
            a_error = "Could not copy settings while removing paths";
            return std::nullopt;
        }
        for (const auto& path : a_paths)
        {
            if (!RemovePath(root, path))
            {
                a_error = std::format("Could not remove setting path {}", path);
                return std::nullopt;
            }
        }
        yyjson_mut_doc_set_root(result.get(), root);
        return Write(result.get(), a_error);
    }

    std::optional<std::string> RemoveLike(
        const std::string_view a_source,
        const std::string_view a_schema,
        std::string& a_error)
    {
        a_error.clear();
        auto source = Parse(a_source, "overlap-removal source", a_error);
        auto schema = source ? Parse(a_schema, "overlap-removal schema", a_error) : nullptr;
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        auto* schemaRoot = schema ? yyjson_doc_get_root(schema.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot) || !yyjson_is_obj(schemaRoot))
        {
            if (a_error.empty()) a_error = "Overlap-removal source and schema must contain JSON objects at the root";
            return std::nullopt;
        }

        MutableDocument result(yyjson_mut_doc_new(nullptr));
        auto* root = yyjson_val_mut_copy(result.get(), sourceRoot);
        if (!root)
        {
            a_error = "Could not copy settings while removing overlaps";
            return std::nullopt;
        }
        RemoveLikeValue(root, schemaRoot);
        yyjson_mut_doc_set_root(result.get(), root);
        return Write(result.get(), a_error);
    }
}  // namespace MPL::JsonOverlay
