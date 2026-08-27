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

        void OnReferenceInitialized(RE::TESObjectREFR* a_reference)
        {
            if (!IsRuntimeReady())
            {
                return;
            }
            PointLightPatcher::InitializeReference(a_reference);
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

        const LumaAPI::ClientCallbacks callbacks{
            .id = "TuningUtil",
            .OnReferenceInitialized = OnReferenceInitialized,
            .OnCellChanging = OnCellChanging,
            .OnCellChanged = OnCellChanged,
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
               api->GetProviderDetailedLogging &&
               api->UpdateProviderDetailedLogging &&
               api->RegisterClient(&callbacks);
    }

    bool GetProviderDetailedLogging(
        const char* a_id,
        bool& a_detailedLogging)
    {
        return api && api->GetProviderDetailedLogging &&
               api->GetProviderDetailedLogging(
                   a_id,
                   std::addressof(a_detailedLogging));
    }

    bool UpdateProviderDetailedLogging(
        const char* a_id,
        const bool a_detailedLogging)
    {
        return api && api->UpdateProviderDetailedLogging &&
               api->UpdateProviderDetailedLogging(
                   a_id,
                   a_detailedLogging);
    }

    void SetRuntimeReady(const bool a_ready)
    {
        runtimeReady.store(a_ready, std::memory_order_release);
    }
}
