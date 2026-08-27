#pragma once

#include <Config/Lighting.h>
#include <TuningUtil.h>

namespace MPL::LightingPatcher
{
    using Settings = TuningUtil::Settings;
    void ApplyDataLoaded();
    void ApplyAllSettings(bool a_commitLightPlacer = true);
    void ReleaseRuntimeState();
    bool ProfilesShareInteriorTarget(const std::string&, const std::string&);
    bool ProfilesShareFilteredLightingTemplateTarget(const std::string&, const std::string&, std::string_view);

}  // namespace MPL::LightingPatcher
