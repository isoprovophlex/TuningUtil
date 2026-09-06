#pragma once

namespace RE
{
    class TESObjectREFR;
}

namespace MPL::ObjectLightingPatcher
{
    void InstallRuntimeEvents();
    void ApplyAllSettings();
    void QueueLoadedReferenceRefresh();
    void QueueReferenceRefresh(RE::TESObjectREFR*);
    void ResetReferenceTracking();
}
