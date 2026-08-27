#pragma once

#include <Config/Lighting.h>

namespace MPL::PointLightPatcher
{
    void Apply(
        const LightingPatcher::PointLightSettings&,
        const WeatherPatcher::HueRanges&,
        std::span<const std::string> a_effectLightingRegionExclusions,
        bool a_commitLightPlacer = true);
    void InstallRuntimeEvents();
    void BeginCell(RE::TESObjectCELL*);
    void ResetCellTracking();
    void RecordCellChangeThread();
    void InitializeReference(RE::TESObjectREFR*);
    void ReleaseRuntimeState();
}  // namespace MPL::PointLightPatcher
