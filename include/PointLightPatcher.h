#pragma once

#include <Config/Lighting.h>
#include <RecordFilter.h>
#include <unordered_map>

namespace MPL::PointLightPatcher
{
    using BaseLightSettingsMap = std::unordered_map<RE::FormID, LightingPatcher::PointLightSettings>;

    struct ReferenceRule
    {
        std::unordered_set<RE::FormID> baseLights;
        RecordFilter::Resolved xemiFilter;
        double fadeMultiplier = 1.0;

        bool operator==(const ReferenceRule&) const = default;
    };

    RE::Color OriginalBaseColor(RE::TESObjectLIGH&);
    void Apply(
        const LightingPatcher::PointLightSettings&,
        const BaseLightSettingsMap&,
        const std::vector<ReferenceRule>&,
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
