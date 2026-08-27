#pragma once

#include <cstdint>

namespace RE
{
    class TESObjectREFR;
}

namespace MPL::XEMIAPI
{
    inline constexpr std::uint32_t kVersion = 6;

    struct ReferenceCallbacks
    {
        const char* id = nullptr;
        void (*OnReferenceEmittanceChanged)(RE::TESObjectREFR*) = nullptr;
    };

    struct Interface
    {
        std::uint32_t version = kVersion;
        // Reserved to preserve compatibility with existing TuningUtil builds.
        void* reserved1 = nullptr;
        void* reserved2 = nullptr;
        void* reserved3 = nullptr;
        bool (*RegisterReferenceClient)(const ReferenceCallbacks*) = nullptr;
    };

    using RequestInterface = const Interface* (*)(std::uint32_t);
}  // namespace MPL::XEMIAPI
