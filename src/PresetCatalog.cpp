#include <PresetCatalog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <format>
#include <memory>
#include <ranges>
#include <system_error>
#include <Windows.h>
#include <yyjson.h>

namespace MPL::PresetCatalog
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

        bool IEquals(const std::string_view a_left, const std::string_view a_right)
        {
            return a_left.size() == a_right.size() &&
                   std::ranges::equal(a_left, a_right, [](const unsigned char a_lhs, const unsigned char a_rhs)
                   {
                       return std::tolower(a_lhs) == std::tolower(a_rhs);
                   });
        }

        std::optional<std::string> StringMember(yyjson_val* a_object, const std::string_view a_name)
        {
            auto* value = yyjson_is_obj(a_object) ?
                              yyjson_obj_getn(a_object, a_name.data(), a_name.size()) :
                              nullptr;
            return yyjson_is_str(value) ?
                       std::optional<std::string>{ std::string(yyjson_get_str(value), yyjson_get_len(value)) } :
                       std::nullopt;
        }

        std::optional<std::string> WriteDocument(yyjson_mut_doc* a_document, std::string& a_error)
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
                a_error = error.msg ? error.msg : "Could not serialize the preset catalog.";
                return std::nullopt;
            }
            std::string result(data, length);
            std::free(data);
            return result;
        }

        std::optional<std::string> WriteValue(yyjson_val* a_value, std::string& a_error)
        {
            MutableDocument output(yyjson_mut_doc_new(nullptr));
            auto* root = output ? yyjson_val_mut_copy(output.get(), a_value) : nullptr;
            if (!root)
            {
                a_error = "Could not copy preset settings from the catalog.";
                return std::nullopt;
            }
            yyjson_mut_doc_set_root(output.get(), root);
            return WriteDocument(output.get(), a_error);
        }

        bool AddString(
            yyjson_mut_doc* a_document,
            yyjson_mut_val* a_object,
            const std::string_view a_name,
            const std::string_view a_value)
        {
            return yyjson_mut_obj_add_strncpy(
                a_document,
                a_object,
                a_name.data(),
                a_value.data(),
                a_value.size());
        }
    }  // namespace

    std::optional<Catalog> Parse(const std::string_view a_json, std::string& a_error)
    {
        a_error.clear();
        Document document(yyjson_read(
            const_cast<char*>(a_json.data()),
            a_json.size(),
            YYJSON_READ_NOFLAG));
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        auto* categories = yyjson_is_obj(root) ? yyjson_obj_get(root, "categories") : nullptr;
        if (!yyjson_is_obj(root))
        {
            a_error = "The preset catalog root must be a JSON object.";
            return std::nullopt;
        }
        if (!yyjson_is_arr(categories))
        {
            a_error = "The preset catalog does not contain a categories array.";
            return std::nullopt;
        }

        Catalog result;
        std::size_t categoryIndex = 0;
        std::size_t categoryMaximum = 0;
        yyjson_val* categoryValue = nullptr;
        yyjson_arr_foreach(categories, categoryIndex, categoryMaximum, categoryValue)
        {
            const auto categoryName = StringMember(categoryValue, "name");
            auto* presets = yyjson_is_obj(categoryValue) ? yyjson_obj_get(categoryValue, "presets") : nullptr;
            if (!categoryName || categoryName->empty() || !yyjson_is_arr(presets))
            {
                a_error = "Every preset category must have a nonempty name and a presets array.";
                return std::nullopt;
            }
            if (FindCategory(result, *categoryName))
            {
                a_error = "Preset category names must be unique case-insensitively.";
                return std::nullopt;
            }

            Category category{ .name = *categoryName };
            std::size_t presetIndex = 0;
            std::size_t presetMaximum = 0;
            yyjson_val* presetValue = nullptr;
            yyjson_arr_foreach(presets, presetIndex, presetMaximum, presetValue)
            {
                const auto presetName = StringMember(presetValue, "name");
                auto* settings = yyjson_is_obj(presetValue) ? yyjson_obj_get(presetValue, "settings") : nullptr;
                if (!presetName || presetName->empty() || !yyjson_is_obj(settings))
                {
                    a_error = "Every preset must have a nonempty name and a settings object.";
                    return std::nullopt;
                }
                if (FindPreset(category, *presetName))
                {
                    a_error = "Preset names must be unique within a category case-insensitively.";
                    return std::nullopt;
                }
                const auto settingsText = WriteValue(settings, a_error);
                if (!settingsText)
                {
                    return std::nullopt;
                }
                category.presets.push_back({ *presetName, *settingsText });
            }
            result.categories.push_back(std::move(category));
        }
        return result;
    }

    std::optional<std::string> Serialize(const Catalog& a_catalog, std::string& a_error)
    {
        a_error.clear();
        MutableDocument document(yyjson_mut_doc_new(nullptr));
        auto* root = document ? yyjson_mut_obj(document.get()) : nullptr;
        auto* categories = document ? yyjson_mut_arr(document.get()) : nullptr;
        if (!root || !categories ||
            !yyjson_mut_obj_add_val(document.get(), root, "categories", categories))
        {
            a_error = "Could not create the preset catalog document.";
            return std::nullopt;
        }

        std::vector<std::string> categoryNames;
        for (const auto& category : a_catalog.categories)
        {
            if (category.name.empty() || std::ranges::any_of(categoryNames, [&](const auto& a_existing)
                    { return IEquals(a_existing, category.name); }))
            {
                a_error = "Preset category names must be nonempty and unique case-insensitively.";
                return std::nullopt;
            }
            categoryNames.push_back(category.name);

            auto* categoryValue = yyjson_mut_obj(document.get());
            auto* presets = yyjson_mut_arr(document.get());
            if (!categoryValue || !presets ||
                !AddString(document.get(), categoryValue, "name", category.name) ||
                !yyjson_mut_obj_add_val(document.get(), categoryValue, "presets", presets) ||
                !yyjson_mut_arr_append(categories, categoryValue))
            {
                a_error = "Could not create a preset category.";
                return std::nullopt;
            }

            std::vector<std::string> presetNames;
            for (const auto& preset : category.presets)
            {
                if (preset.name.empty() || std::ranges::any_of(presetNames, [&](const auto& a_existing)
                        { return IEquals(a_existing, preset.name); }))
                {
                    a_error = "Preset names must be nonempty and unique within a category case-insensitively.";
                    return std::nullopt;
                }
                presetNames.push_back(preset.name);

                Document settingsDocument(yyjson_read(
                    const_cast<char*>(preset.settings.data()),
                    preset.settings.size(),
                    YYJSON_READ_NOFLAG));
                auto* settingsRoot = settingsDocument ? yyjson_doc_get_root(settingsDocument.get()) : nullptr;
                auto* settings = yyjson_is_obj(settingsRoot) ?
                                     yyjson_val_mut_copy(document.get(), settingsRoot) :
                                     nullptr;
                auto* presetValue = yyjson_mut_obj(document.get());
                if (!settings || !presetValue ||
                    !AddString(document.get(), presetValue, "name", preset.name) ||
                    !yyjson_mut_obj_add_val(document.get(), presetValue, "settings", settings) ||
                    !yyjson_mut_arr_append(presets, presetValue))
                {
                    a_error = "Every preset settings value must be a valid JSON object.";
                    return std::nullopt;
                }
            }
        }

        yyjson_mut_doc_set_root(document.get(), root);
        return WriteDocument(document.get(), a_error);
    }

    std::optional<Catalog> Read(const std::filesystem::path& a_path, std::string& a_error)
    {
        a_error.clear();
        std::ifstream file(a_path, std::ios::binary);
        if (!file)
        {
            a_error = "The preset catalog could not be opened.";
            return std::nullopt;
        }
        std::string text(std::istreambuf_iterator<char>(file), {});
        constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";
        if (text.starts_with(utf8Bom)) text.erase(0, utf8Bom.size());
        auto result = Parse(text, a_error);
        if (!result && !a_error.empty())
        {
            a_error = a_path.string() + ": " + a_error;
        }
        return result;
    }

    bool Write(
        const std::filesystem::path& a_path,
        const Catalog& a_catalog,
        std::string& a_error)
    {
        const auto text = Serialize(a_catalog, a_error);
        if (!text)
        {
            return false;
        }

        auto temporaryPath = a_path;
        temporaryPath += ".tmp";
        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            file << *text << '\n';
            if (!file)
            {
                file.close();
                std::error_code removeError;
                std::filesystem::remove(temporaryPath, removeError);
                a_error = "The temporary preset catalog could not be written.";
                return false;
            }
        }
        if (::MoveFileExW(
                temporaryPath.c_str(),
                a_path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return true;
        }

        const std::error_code moveError(static_cast<int>(::GetLastError()), std::system_category());
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        a_error = "The preset catalog could not be replaced: " + moveError.message();
        return false;
    }

    Category* FindCategory(Catalog& a_catalog, const std::string_view a_name)
    {
        const auto found = std::ranges::find_if(a_catalog.categories, [&](const auto& a_category)
            { return IEquals(a_category.name, a_name); });
        return found == a_catalog.categories.end() ? nullptr : std::addressof(*found);
    }

    const Category* FindCategory(const Catalog& a_catalog, const std::string_view a_name)
    {
        const auto found = std::ranges::find_if(a_catalog.categories, [&](const auto& a_category)
            { return IEquals(a_category.name, a_name); });
        return found == a_catalog.categories.end() ? nullptr : std::addressof(*found);
    }

    Preset* FindPreset(Category& a_category, const std::string_view a_name)
    {
        const auto found = std::ranges::find_if(a_category.presets, [&](const auto& a_preset)
            { return IEquals(a_preset.name, a_name); });
        return found == a_category.presets.end() ? nullptr : std::addressof(*found);
    }

    const Preset* FindPreset(const Category& a_category, const std::string_view a_name)
    {
        const auto found = std::ranges::find_if(a_category.presets, [&](const auto& a_preset)
            { return IEquals(a_preset.name, a_name); });
        return found == a_category.presets.end() ? nullptr : std::addressof(*found);
    }
}  // namespace MPL::PresetCatalog
