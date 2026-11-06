#pragma once

#include <Config/Lighting.h>
#include <TuningUtil.h>
#include <array>

namespace MPL::LightingPatcher
{
    using Settings = TuningUtil::Settings;

    inline std::array<bool, 5> CellActiveFields(const RE::INTERIOR_DATA& a_data, const bool a_hasLightingTemplate)
    {
        if (!a_hasLightingTemplate) return { true, true, true, true, true };
        const auto& inherit = a_data.lightingTemplateInheritanceFlags;
        const auto ambientActive = (inherit & RE::INTERIOR_DATA::Inherit::kAmbientColor).underlying() == 0;
        return {
            ambientActive,
            (inherit & RE::INTERIOR_DATA::Inherit::kDirectionalColor).underlying() == 0,
            ambientActive,
            (inherit & RE::INTERIOR_DATA::Inherit::kFogColor).underlying() == 0,
            (inherit & RE::INTERIOR_DATA::Inherit::kFogColor).underlying() == 0,
        };
    }

    void ApplyDataLoaded();
    void ApplyAllSettings(bool a_commitLightPlacer = true);
    void ApplyTemplateSettings();
    void ApplyPointLightSettings(bool a_commitLightPlacer = true);
    void ReleaseRuntimeState();
    bool ProfilesShareLightingTarget(const std::string&, const std::string&);
    bool ProfilesShareFilteredLightingTemplateTarget(const std::string&, const std::string&, std::string_view);
    bool ProfilesShareFilteredBaseLightTarget(const std::string&, const std::string&, std::string_view);

}  // namespace MPL::LightingPatcher
