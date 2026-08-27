#pragma once

#include <WeatherSyncAPI.h>

namespace RE
{
    class TESWeather;
}

namespace MPL::WeatherSyncClient
{
    bool Load();
    WeatherSyncAPI::SetWeatherResult SetWeatherInstant(
        RE::TESWeather*,
        bool);
}  // namespace MPL::WeatherSyncClient
