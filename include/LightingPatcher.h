#pragma once

#include <Config/Lighting.h>
#include <TuningUtil.h>

namespace MPL::LightingPatcher
{
    using Settings = TuningUtil::Settings;
    void CaptureCellBaseline(RE::TESObjectCELL*);
    void ApplyDataLoaded();
    void ApplyAllSettings(bool a_commitLightPlacer = true);
    void ReleaseRuntimeState();

}  // namespace MPL::LightingPatcher
