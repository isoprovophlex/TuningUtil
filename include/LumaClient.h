#pragma once

#include <LumaAPI.h>

namespace MPL::LumaClient
{
    bool Load();
    bool GetProviderSettings(const char*, bool&, bool&);
    bool UpdateProviderSettings(
        const char*,
        std::int8_t,
        std::int8_t);
    void SetRuntimeReady(bool);
}
