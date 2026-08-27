#pragma once

#include <Config/Tuning.h>
#include <unordered_set>

namespace MPL::RecordFilter
{
    struct Resolved
    {
        std::unordered_set<RE::FormID> includedFormIDs;
        std::unordered_set<RE::FormID> excludedFormIDs;
        std::vector<std::string> includedEditorIDFragments;
        std::vector<std::string> excludedEditorIDFragments;
        TuningUtil::PluginFilter includedPlugins;
        TuningUtil::PluginFilter excludedPlugins;
    };

    Resolved Resolve(
        const TuningUtil::WeatherFilter&,
        const TuningUtil::WeatherFilter&,
        const TuningUtil::PluginFilter&,
        const TuningUtil::PluginFilter&);
    bool Matches(const RE::TESForm*, const Resolved&);
    std::string EditorID(const RE::TESForm*);
    std::string DisplayName(const RE::TESForm*);
    std::string FormKey(const RE::TESForm*);
}  // namespace MPL::RecordFilter
