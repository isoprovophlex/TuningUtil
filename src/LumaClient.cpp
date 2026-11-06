#include <Config.h>
#include <DetailedLogging.h>
#include <ImageSpaceDiagnostics.h>
#include <LumaClient.h>
#include <MMSFProviderSettings.h>
#include <ObjectLightingPatcher.h>
#include <PointLightPatcher.h>

#include <atomic>

namespace MPL::LumaClient
{
    namespace
    {
        API::Luma::ILumaPluginService* api = nullptr;
        API::MMSF::Interface* registry = nullptr;
        std::atomic_bool runtimeReady{ false };

        bool IsRuntimeReady()
        {
            return runtimeReady.load(std::memory_order_acquire);
        }

        void OnReferenceInitialized(RE::TESObjectREFR* a_reference)
        {
            if (!IsRuntimeReady())
            {
                return;
            }
            PointLightPatcher::QueueReferenceReconciliation(a_reference);
            ObjectLightingPatcher::QueueReferenceRefresh(a_reference);
        }

        void OnCellChanging(RE::TESObjectCELL* a_cell)
        {
            DetailedLogging::Info("[Cell Diagnostic] event=changing-begin | cell={:08X} | runtimeReady={}",
                a_cell ? a_cell->GetFormID() : 0, IsRuntimeReady());
            if (DetailedLogging::IsEnabled()) spdlog::default_logger()->flush();
            if (IsRuntimeReady())
            {
                PointLightPatcher::BeginCell(a_cell);
            }
            DetailedLogging::Info("[Cell Diagnostic] event=changing-end | cell={:08X}",
                a_cell ? a_cell->GetFormID() : 0);
        }

        void OnCellChanged(const RE::TESObjectCELL* a_cell)
        {
            DetailedLogging::Info("[Cell Diagnostic] event=changed-begin | cell={:08X}",
                a_cell ? a_cell->GetFormID() : 0);
            if (DetailedLogging::IsEnabled()) spdlog::default_logger()->flush();
            DetailedLogging::Info("[Cell Diagnostic] event=changed-end | cell={:08X}",
                a_cell ? a_cell->GetFormID() : 0);
            ImageSpaceDiagnostics::QueueSnapshot("cell-changed", a_cell ? a_cell->GetFormID() : 0,
                DetailedLogging::IsEnabled);
        }

        const API::Luma::ClientCallbacks callbacks{
            .id = "TuningUtil",
            .OnReferenceInitialized = OnReferenceInitialized,
            .OnCellChanging = OnCellChanging,
            .OnCellChanged = OnCellChanged,
        };
    }

    bool Load()
    {
        if (api) return true;
        auto* mmsf = Config::StatData::GetSingleton()->GetMMSFAPI();
        auto* service = mmsf ? mmsf->QueryService("LUMA") : nullptr;
        if (!service || service->GetVersion() != API::Luma::kVersion) return false;

        auto* luma = static_cast<API::Luma::ILumaPluginService*>(service);
        if (!luma->RegisterClient(&callbacks)) return false;
        registry = mmsf;
        api = luma;
        return true;
    }

    bool GetProviderDetailedLogging(
        const char* a_id,
        bool& a_detailedLogging)
    {
        return MMSFProviderSettings::ReadDetailedLogging(registry, a_id, a_detailedLogging);
    }

    bool UpdateProviderDetailedLogging(
        const char* a_id,
        const bool a_detailedLogging)
    {
        return MMSFProviderSettings::WriteDetailedLogging(registry, a_id, a_detailedLogging);
    }

    void SetRuntimeReady(const bool a_ready)
    {
        runtimeReady.store(a_ready, std::memory_order_release);
    }
}
