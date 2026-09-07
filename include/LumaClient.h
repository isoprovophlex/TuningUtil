#pragma once

#include <LumaServiceAPI.h>

namespace MPL::LumaClient
{
    bool Load();
    bool GetProviderDetailedLogging(const char*, bool&);
    bool UpdateProviderDetailedLogging(const char*, bool);
    void SetRuntimeReady(bool);
}
