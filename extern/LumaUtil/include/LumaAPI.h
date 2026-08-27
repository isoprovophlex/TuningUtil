#pragma once

#include <cstdint>

namespace RE
{
    class TESObjectCELL;
    class TESObjectREFR;
}  // namespace RE

namespace MPL::LumaAPI
{
    inline constexpr std::uint32_t kVersion = 5;

    struct ClientCallbacks
    {
        // The ID is copied during registration.
        const char* id = nullptr;
        void (*OnCellInitialized)(RE::TESObjectCELL*) = nullptr;
        void (*OnReferenceInitialized)(RE::TESObjectREFR*) = nullptr;
        void (*OnCellChanging)(RE::TESObjectCELL*) = nullptr;
        void (*OnCellChanged)(const RE::TESObjectCELL*) = nullptr;
        // The provider string is borrowed and valid only during the callback.
        void (*OnCellPatched)(
            RE::TESObjectCELL*,
            const char*,
            bool) = nullptr;
    };

    struct Interface
    {
        std::uint32_t version = kVersion;
        bool (*RegisterClient)(const ClientCallbacks*) = nullptr;
        bool (*GetProviderDetailedLogging)(const char*, bool*) = nullptr;
        bool (*UpdateProviderDetailedLogging)(const char*, bool) = nullptr;
    };

    using RequestInterface = const Interface* (*) (std::uint32_t);
}  // namespace MPL::LumaAPI
