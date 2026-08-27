#include <Config.h>
#include <RecordFilter.h>
#include <algorithm>
#include <cctype>
#include <ranges>

namespace MPL::RecordFilter
{
    namespace
    {
        std::string Lowercase(std::string a_value)
        {
            std::ranges::transform(
                a_value,
                a_value.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return a_value;
        }

        bool FilterEmpty(const TuningUtil::PluginFilter& a_filter)
        {
            return a_filter.exact.empty() && a_filter.contains.empty();
        }

        bool PluginNameMatches(
            const std::string_view a_pluginName,
            const TuningUtil::PluginFilter& a_filter)
        {
            if (std::ranges::any_of(a_filter.exact, [&](const auto& a_name)
                    { return Config::IEquals(a_pluginName, a_name); }))
            {
                return true;
            }

            const auto pluginName = Lowercase(std::string(a_pluginName));
            return std::ranges::any_of(a_filter.contains, [&](const auto& a_fragment)
            {
                const auto fragment = Lowercase(a_fragment);
                return !fragment.empty() && pluginName.contains(fragment);
            });
        }

        bool MatchesPluginFilter(
            const RE::TESForm* a_form,
            const TuningUtil::PluginFilter& a_filter)
        {
            if (!a_form || FilterEmpty(a_filter))
            {
                return false;
            }
            if (a_form->sourceFiles.array)
            {
                for (const auto* sourceFile : *a_form->sourceFiles.array)
                {
                    if (sourceFile && PluginNameMatches(sourceFile->GetFilename(), a_filter))
                    {
                        return true;
                    }
                }
            }
            const auto* winningFile = a_form->GetFile();
            return winningFile && PluginNameMatches(winningFile->GetFilename(), a_filter);
        }

        bool EditorIDContains(
            const RE::TESForm* a_form,
            const std::span<const std::string> a_fragments)
        {
            const auto editorID = Lowercase(EditorID(a_form));
            return !editorID.empty() && std::ranges::any_of(a_fragments, [&](const auto& a_fragment)
            {
                const auto fragment = Lowercase(a_fragment);
                return !fragment.empty() && editorID.contains(fragment);
            });
        }

        std::unordered_set<RE::FormID> ResolveFormIDs(
            const TuningUtil::WeatherFilter& a_filter)
        {
            std::unordered_set<RE::FormID> result;
            for (const auto& configured : a_filter.formIDs)
            {
                const auto formID = Config::LiteForm::FromString(configured).formID;
                if (formID != 0)
                {
                    result.insert(formID);
                }
            }
            return result;
        }
    }  // namespace

    Resolved Resolve(
        const TuningUtil::WeatherFilter& a_includedRecords,
        const TuningUtil::WeatherFilter& a_excludedRecords,
        const TuningUtil::PluginFilter& a_includedPlugins,
        const TuningUtil::PluginFilter& a_excludedPlugins)
    {
        return {
            .includedFormIDs = ResolveFormIDs(a_includedRecords),
            .excludedFormIDs = ResolveFormIDs(a_excludedRecords),
            .includedEditorIDFragments = a_includedRecords.contains,
            .excludedEditorIDFragments = a_excludedRecords.contains,
            .includedPlugins = a_includedPlugins,
            .excludedPlugins = a_excludedPlugins,
        };
    }

    bool Matches(const RE::TESForm* a_form, const Resolved& a_filter)
    {
        if (!a_form ||
            a_filter.excludedFormIDs.contains(a_form->GetFormID()) ||
            EditorIDContains(a_form, a_filter.excludedEditorIDFragments) ||
            MatchesPluginFilter(a_form, a_filter.excludedPlugins))
        {
            return false;
        }

        const auto hasRecordInclusions =
            !a_filter.includedFormIDs.empty() ||
            !a_filter.includedEditorIDFragments.empty();
        const auto recordIncluded =
            !hasRecordInclusions ||
            a_filter.includedFormIDs.contains(a_form->GetFormID()) ||
            EditorIDContains(a_form, a_filter.includedEditorIDFragments);
        const auto pluginIncluded =
            FilterEmpty(a_filter.includedPlugins) ||
            MatchesPluginFilter(a_form, a_filter.includedPlugins);
        return recordIncluded && pluginIncluded;
    }

    std::string EditorID(const RE::TESForm* a_form)
    {
        if (!a_form)
        {
            return {};
        }
        auto* stat = Config::StatData::GetSingleton();
        if (!stat->mmsfAPI)
        {
            stat->mmsfAPI = API::RequestMMSFAPI();
        }
        if (stat->mmsfAPI)
        {
            if (auto editorID = stat->mmsfAPI->LookupEDIDForFormID(a_form->GetFormID());
                !editorID.empty())
            {
                return editorID;
            }
        }
        const auto* editorID = a_form->GetFormEditorID();
        return editorID ? editorID : "";
    }

    std::string DisplayName(const RE::TESForm* a_form)
    {
        auto editorID = EditorID(a_form);
        return editorID.empty() ? "<no editor ID>" : std::move(editorID);
    }

    std::string FormKey(const RE::TESForm* a_form)
    {
        if (!a_form)
        {
            return {};
        }
        auto* sourceFile = a_form->GetFile(0);
        if (!sourceFile)
        {
            sourceFile = a_form->GetFile();
        }
        return sourceFile ?
                   std::format("{:06X}:{}", a_form->GetLocalFormID(), sourceFile->GetFilename()) :
                   std::format("{:08X}", a_form->GetFormID());
    }
}  // namespace MPL::RecordFilter
