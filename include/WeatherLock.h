#pragma once

namespace MPL::WeatherLock
{
    void InstallHooks();
    void SetSelectedWeather(RE::TESWeather* a_weather);
    [[nodiscard]] RE::TESWeather* GetSelectedWeather();
    void ReleaseOverride();
    void SetEnabled(bool a_enabled);
    [[nodiscard]] bool IsEnabled();
    bool Maintain(bool a_force = false);
}  // namespace MPL::WeatherLock
