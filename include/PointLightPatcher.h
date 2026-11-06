#pragma once

#include <Config/Lighting.h>
#include <RecordFilter.h>
#include <optional>
#include <string>
#include <unordered_map>

namespace MPL::PointLightPatcher
{
    using BaseLightSettingsMap = std::unordered_map<RE::FormID, LightingPatcher::PointLightSettings>;

    struct ReferenceRule
    {
        std::string profile;
        int priority = 0;
        bool useXemiFilter = false;
        std::unordered_set<RE::FormID> baseLights;
        RecordFilter::Resolved xemiFilter;
        std::optional<double> fadeMultiplier;
        std::optional<double> radiusMultiplier;

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
    void QueueReferenceReconciliation(RE::TESObjectREFR*);
    void InitializeReference(RE::TESObjectREFR*);
    void ReleaseRuntimeState();
}  // namespace MPL::PointLightPatcher
