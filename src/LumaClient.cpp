#include <Config.h>
#include <LumaClient.h>
#include <ObjectLightingPatcher.h>
#include <PointLightPatcher.h>

#include <atomic>

namespace MPL::LumaClient
{
    namespace
    {
        API::Luma::ILumaPluginService* api = nullptr;
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
            if (IsRuntimeReady())
            {
                PointLightPatcher::BeginCell(a_cell);
            }
        }

        void OnCellChanged(const RE::TESObjectCELL*)
        {
            PointLightPatcher::RecordCellChangeThread();
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
        api = luma;
        return true;
    }

    bool GetProviderDetailedLogging(
        const char* a_id,
        bool& a_detailedLogging)
    {
        return api &&
               api->GetProviderSettings(
                   a_id,
                   std::addressof(a_detailedLogging),
                   nullptr);
    }

    bool UpdateProviderDetailedLogging(
        const char* a_id,
        const bool a_detailedLogging)
    {
        return api &&
               api->UpdateProviderSettings(
                   a_id,
                   a_detailedLogging ?
                       std::int8_t{ 1 } :
                       std::int8_t{ 0 },
                   std::int8_t{ -1 });
    }

    void SetRuntimeReady(const bool a_ready)
    {
        runtimeReady.store(a_ready, std::memory_order_release);
    }
}
