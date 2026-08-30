#pragma once

#include <Config/Lighting.h>
#include <RecordFilter.h>
#include <unordered_map>

namespace MPL::PointLightPatcher
{
    using BaseLightSettingsMap = std::unordered_map<RE::FormID, LightingPatcher::PointLightSettings>;

    void Apply(
        const LightingPatcher::PointLightSettings&,
        const BaseLightSettingsMap&,
        const RecordFilter::Resolved&,
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
