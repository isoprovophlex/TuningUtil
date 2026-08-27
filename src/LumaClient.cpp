#include <LightingPatcher.h>
#include <LumaClient.h>
#include <PointLightPatcher.h>

#include <atomic>

namespace MPL::LumaClient
{
    namespace
    {
        HMODULE module = nullptr;
        const LumaAPI::Interface* api = nullptr;
        std::atomic_bool runtimeReady{ false };

        bool IsRuntimeReady()
        {
            return runtimeReady.load(std::memory_order_acquire);
        }

        void OnCellInitialized(RE::TESObjectCELL* a_cell)
        {
            if (IsRuntimeReady()) LightingPatcher::CaptureCellBaseline(a_cell);
        }

        void OnReferenceInitialized(RE::TESObjectREFR* a_reference)
        {
            if (IsRuntimeReady()) PointLightPatcher::InitializeReference(a_reference);
        }

        const LumaAPI::ClientCallbacks callbacks{
            .id = "TuningUtil",
            .OnCellInitialized = OnCellInitialized,
            .OnReferenceInitialized = OnReferenceInitialized,
        };
    }

    bool Load()
    {
        module = GetModuleHandleW(L"LumaUtil.dll");
        const auto request = module ?
                                 reinterpret_cast<LumaAPI::RequestInterface>(
                                     GetProcAddress(module, "LumaUtil_RequestAPI")) :
                                 nullptr;
        api = request ? request(LumaAPI::kVersion) : nullptr;
        return api && api->version == LumaAPI::kVersion &&
               api->RegisterClient &&
               api->GetProviderSettings &&
               api->UpdateProviderSettings &&
               api->RegisterClient(&callbacks);
    }

    bool GetProviderSettings(
        const char* a_id,
        bool& a_detailedLogging,
        bool& a_notifications)
    {
        return api && api->GetProviderSettings &&
               api->GetProviderSettings(
                   a_id,
                   std::addressof(a_detailedLogging),
                   std::addressof(a_notifications));
    }

    bool UpdateProviderSettings(
        const char* a_id,
        const std::int8_t a_detailedLogging,
        const std::int8_t a_notifications)
    {
        return api && api->UpdateProviderSettings &&
               api->UpdateProviderSettings(
                   a_id,
                   a_detailedLogging,
                   a_notifications);
    }

    void SetRuntimeReady(const bool a_ready)
    {
        runtimeReady.store(a_ready, std::memory_order_release);
    }
}
