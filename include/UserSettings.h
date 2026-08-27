#pragma once

#include <PresetCatalog.h>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace MPL::UserSettings
{
    using PresetSelections = std::map<std::string, std::string>;

    struct SanitizeResult
    {
        std::string text;
        bool settingsChanged = false;
        bool presetSelectionsChanged = false;
        std::size_t removedPresetSelections = 0;

        bool Changed() const { return settingsChanged || presetSelectionsChanged; }
    };

    std::optional<std::string> ValuesOnly(std::string_view, std::string&);
    std::optional<PresetSelections> ParsePresetSelections(std::string_view, std::string&);
    std::string PresetSelectionsText(const PresetSelections&);
    std::optional<SanitizeResult> Sanitize(
        std::string_view,
        std::string_view,
        const PresetCatalog::Catalog*,
        std::string&);
}  // namespace MPL::UserSettings
