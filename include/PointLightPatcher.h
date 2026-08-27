#pragma once

#include <Config/Lighting.h>
#include <unordered_map>
#include <unordered_set>

namespace MPL::PointLightPatcher
{
    using BaseLightSettingsMap = std::unordered_map<RE::FormID, LightingPatcher::PointLightSettings>;
    using SunlightBaseLights = std::unordered_set<RE::FormID>;

    void Apply(
        const LightingPatcher::PointLightSettings&,
        const BaseLightSettingsMap&,
        const SunlightBaseLights&,
        const WeatherPatcher::HueRanges&,
        bool a_commitLightPlacer = true);
    void InstallRuntimeEvents();
    void BeginCell(RE::TESObjectCELL*);
    void ResetCellTracking();
    void RecordCellChangeThread();
    void QueueReferenceReconciliation(RE::TESObjectREFR*);
    void InitializeReference(RE::TESObjectREFR*);
    void ReleaseRuntimeState();
}  // namespace MPL::PointLightPatcher
