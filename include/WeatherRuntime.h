#pragma once

#include <cstdint>

namespace RE
{
    class TESWeather;
}

namespace MPL::WeatherRuntime
{
    enum class SetWeatherStatus : std::uint32_t
    {
        kInvalidWeather,
        kSkyUnavailable,
        kAppliedWithoutPlayerCell,
        kAppliedWithoutLoadedData,
        kAppliedAndRefreshed,
    };

    struct SetWeatherResult
    {
        SetWeatherStatus status = SetWeatherStatus::kInvalidWeather;
        std::uint32_t lightCount = 0;
    };

    SetWeatherResult SetWeatherInstant(RE::TESWeather*, bool);
    std::uint32_t RefreshCurrentCellEmittance();
}  // namespace MPL::WeatherRuntime
