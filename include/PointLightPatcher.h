#pragma once

#include <Config/Lighting.h>

namespace MPL::PointLightPatcher
{
    void Apply(
        const LightingPatcher::PointLightSettings&,
        const WeatherPatcher::HueRanges&,
        bool a_commitLightPlacer = true);
    void InstallRuntimeEvents();
    void InitializeReference(RE::TESObjectREFR*);
    void ReleaseRuntimeState();
}  // namespace MPL::PointLightPatcher
