#include <UserSettings.h>

#include <JsonOverlay.h>
#include <memory>
#include <rfl/json.hpp>
#include <vector>
#include <yyjson.h>

namespace MPL::UserSettings
{
    namespace
    {
        constexpr std::string_view kPresetSelectionsMember = "presetSelections";
        const std::vector<std::string> kMetadataPaths{ std::string(kPresetSelectionsMember) };

        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const { yyjson_doc_free(a_document); }
        };

        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;

        Document Parse(const std::string_view a_json)
        {
            return Document(yyjson_read(
                const_cast<char*>(a_json.data()),
                a_json.size(),
                YYJSON_READ_NOFLAG));
        }

        std::string Trim(std::string a_value)
        {
            const auto first = a_value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return {};
            const auto last = a_value.find_last_not_of(" \t\r\n");
            return a_value.substr(first, last - first + 1);
        }
    }  // namespace

    std::optional<std::string> ValuesOnly(
        const std::string_view a_json,
        std::string& a_error)
    {
        return JsonOverlay::RemovePaths(a_json, kMetadataPaths, a_error);
    }

    std::optional<PresetSelections> ParsePresetSelections(
        const std::string_view a_json,
        std::string& a_error)
    {
        a_error.clear();
        const auto document = Parse(a_json);
        auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
        if (!yyjson_is_obj(root))
        {
            a_error = "The user settings file is not a JSON object.";
            return std::nullopt;
        }

        auto* selections = yyjson_obj_getn(
            root,
            kPresetSelectionsMember.data(),
            kPresetSelectionsMember.size());
        if (!selections) return PresetSelections{};
        if (!yyjson_is_obj(selections))
        {
            a_error = "presetSelections must be a JSON object.";
            return std::nullopt;
        }

        PresetSelections result;
        std::size_t index = 0;
        std::size_t maximum = 0;
        yyjson_val* category = nullptr;
        yyjson_val* preset = nullptr;
        yyjson_obj_foreach(selections, index, maximum, category, preset)
        {
            if (!yyjson_is_str(category) || !yyjson_is_str(preset))
            {
                a_error = "Every presetSelections entry must map a category to a preset name.";
                return std::nullopt;
            }
            auto categoryName = Trim(std::string(yyjson_get_str(category), yyjson_get_len(category)));
            auto presetName = Trim(std::string(yyjson_get_str(preset), yyjson_get_len(preset)));
            if (categoryName.empty() || presetName.empty())
            {
                a_error = "Preset selection category and preset names cannot be empty.";
                return std::nullopt;
            }
            result.insert_or_assign(std::move(categoryName), std::move(presetName));
        }
        return result;
    }

    std::string PresetSelectionsText(const PresetSelections& a_selections)
    {
        struct Metadata
        {
            PresetSelections presetSelections;
        };
        return rfl::json::write(Metadata{ a_selections }, rfl::json::pretty);
    }

    std::optional<SanitizeResult> Sanitize(
        const std::string_view a_json,
        const std::string_view a_settingsSchema,
        const PresetCatalog::Catalog* a_presetCatalog,
        std::string& a_error)
    {
        a_error.clear();
        const auto values = ValuesOnly(a_json, a_error);
        const auto sanitizedValues = values ?
                                         JsonOverlay::ProjectLike(*values, a_settingsSchema, a_error) :
                                         std::nullopt;
        const auto selections = sanitizedValues ? ParsePresetSelections(a_json, a_error) : std::nullopt;
        if (!values || !sanitizedValues || !selections) return std::nullopt;

        PresetSelections sanitizedSelections;
        auto removedPresetSelections = std::size_t{ 0 };
        if (a_presetCatalog)
        {
            for (const auto& [categoryName, presetName] : *selections)
            {
                const auto* category = PresetCatalog::FindCategory(*a_presetCatalog, categoryName);
                const auto* preset = category ? PresetCatalog::FindPreset(*category, presetName) : nullptr;
                if (!category || !preset)
                {
                    ++removedPresetSelections;
                    continue;
                }
                sanitizedSelections.insert_or_assign(category->name, preset->name);
            }
        }
        else
        {
            sanitizedSelections = *selections;
        }

        const auto settingsEquivalent = JsonOverlay::Equivalent(*values, *sanitizedValues, a_error);
        const auto output = settingsEquivalent ?
                                JsonOverlay::Overlay(
                                    *sanitizedValues,
                                    PresetSelectionsText(sanitizedSelections),
                                    a_error) :
                                std::nullopt;
        if (!settingsEquivalent || !output) return std::nullopt;

        return SanitizeResult{
            .text = *output,
            .settingsChanged = !*settingsEquivalent,
            .presetSelectionsChanged = sanitizedSelections != *selections,
            .removedPresetSelections = removedPresetSelections,
        };
    }
}  // namespace MPL::UserSettings
