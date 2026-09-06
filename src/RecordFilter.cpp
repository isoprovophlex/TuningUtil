#include <Config.h>
#include <RecordFilter.h>
#include <RecordFilterLogic.h>
#include <algorithm>
#include <cctype>
#include <ranges>
#include <unordered_map>

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

        std::unordered_set<RE::FormID> ResolveFormIDs(
            const std::span<const std::string> a_configuredFormIDs)
        {
            std::unordered_set<RE::FormID> result;
            for (const auto& configured : a_configuredFormIDs)
            {
                const auto formID = Config::LiteForm::FromString(configured).formID;
                if (formID != 0)
                {
                    result.insert(formID);
                }
            }
            return result;
        }

        const std::unordered_map<RE::FormID, std::string>& RuntimeEditorIDs()
        {
            static const auto editorIDs = []
            {
                std::unordered_map<RE::FormID, std::string> result;
                const auto& [forms, lock] = RE::TESForm::GetAllFormsByEditorID();
                const RE::BSReadLockGuard guard{ lock };
                if (!forms)
                {
                    return result;
                }
                result.reserve(forms->size());
                for (const auto& [editorID, form] : *forms)
                {
                    if (form && !editorID.empty())
                    {
                        result.try_emplace(form->GetFormID(), editorID.c_str());
                    }
                }
                return result;
            }();
            return editorIDs;
        }
    }  // namespace

    Resolved Resolve(
        const TuningUtil::WeatherFilter& a_includedRecords,
        const TuningUtil::WeatherFilter& a_excludedRecords,
        const TuningUtil::PluginFilter& a_includedPlugins,
        const TuningUtil::PluginFilter& a_excludedPlugins)
    {
        return {
            .includedFormIDs = ResolveFormIDs(a_includedRecords.formIDs),
            .excludedFormIDs = ResolveFormIDs(a_excludedRecords.formIDs),
            .includedEditorIDFragments = a_includedRecords.contains,
            .excludedEditorIDFragments = a_excludedRecords.contains,
            .includedPlugins = a_includedPlugins,
            .excludedPlugins = a_excludedPlugins,
        };
    }

    Resolved Resolve(
        const std::span<const std::string> a_includedFormIDs,
        const std::span<const std::string> a_excludedFormIDs,
        const TuningUtil::PluginFilter& a_includedPlugins,
        const TuningUtil::PluginFilter& a_excludedPlugins)
    {
        return {
            .includedFormIDs = ResolveFormIDs(a_includedFormIDs),
            .excludedFormIDs = ResolveFormIDs(a_excludedFormIDs),
            .includedPlugins = a_includedPlugins,
            .excludedPlugins = a_excludedPlugins,
        };
    }

    bool Matches(const RE::TESForm* a_form, const Resolved& a_filter)
    {
        if (!a_form ||
            !RecordFilterLogic::MatchesRecord(a_form->GetFormID(), a_filter,
                [&] { return EditorID(a_form); }) ||
            MatchesPluginFilter(a_form, a_filter.excludedPlugins))
        {
            return false;
        }

        return FilterEmpty(a_filter.includedPlugins) ||
            MatchesPluginFilter(a_form, a_filter.includedPlugins);
    }

    std::string EditorID(const RE::TESForm* a_form)
    {
        if (!a_form)
        {
            return {};
        }
        if (a_form->Is(RE::FormType::Weather) || a_form->Is(RE::FormType::Region))
        {
            auto* stat = Config::StatData::GetSingleton();
            if (!stat->mmsfAPI)
            {
                stat->mmsfAPI = API::MMSF::RequestMMSFAPI();
            }
            if (!stat->edidCache)
            {
                stat->edidCache = static_cast<MPL::API::MMSF::IEDIDCache*>(stat->mmsfAPI->QueryService("EDID"));
            }
            if (stat->mmsfAPI)
            {
                if (auto editorID = stat->edidCache->LookupFormID(a_form->GetFormID());
                    !editorID.empty() && !Config::IEquals(editorID, "ERR"))
                {
                    return editorID;
                }
            }
        }
        if (const auto* editorID = a_form->GetFormEditorID(); editorID && *editorID)
        {
            return editorID;
        }
        const auto& runtimeEditorIDs = RuntimeEditorIDs();
        const auto editorID = runtimeEditorIDs.find(a_form->GetFormID());
        return editorID != runtimeEditorIDs.end() ? editorID->second : std::string{};
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
