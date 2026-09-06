#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

namespace MPL::RecordFilterLogic
{
    inline bool ContainsAny(
        const std::string_view a_editorID,
        const std::span<const std::string> a_fragments)
    {
        return std::ranges::any_of(a_fragments, [&](const auto& a_fragment)
        {
            return !a_fragment.empty() && !std::ranges::search(a_editorID, a_fragment,
                [](const unsigned char a_left, const unsigned char a_right)
                { return std::tolower(a_left) == std::tolower(a_right); }).empty();
        });
    }

    template <class Filter, class EditorIDLookup>
    bool MatchesRecord(
        const std::uint32_t a_formID,
        const Filter& a_filter,
        const EditorIDLookup& a_lookup)
    {
        if (a_filter.excludedFormIDs.contains(a_formID)) return false;
        std::optional<std::string> editorID;
        const auto contains = [&](const auto& a_fragments)
        {
            if (a_fragments.empty()) return false;
            if (!editorID) editorID = a_lookup();
            return ContainsAny(*editorID, a_fragments);
        };
        if (contains(a_filter.excludedEditorIDFragments)) return false;
        return (!a_filter.requireIncludedRecordMatch &&
                   a_filter.includedFormIDs.empty() &&
                   a_filter.includedEditorIDFragments.empty()) ||
               a_filter.includedFormIDs.contains(a_formID) ||
               contains(a_filter.includedEditorIDFragments);
    }
}  // namespace MPL::RecordFilterLogic
