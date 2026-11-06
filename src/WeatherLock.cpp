#include <WeatherLock.h>
#include <WeatherRuntime.h>
#include <atomic>

namespace MPL::WeatherLock
{
    namespace
    {
        std::atomic<RE::TESWeather*> selectedWeather{ nullptr };
        std::atomic_bool enabled{ false };

        RE::TESWeather* LockedWeather()
        {
            return enabled.load(std::memory_order_acquire) ? selectedWeather.load(std::memory_order_acquire) : nullptr;
        }
    }  // namespace

    void SetSelectedWeather(RE::TESWeather* a_weather)
    {
        selectedWeather.store(a_weather, std::memory_order_release);
    }

    RE::TESWeather* GetSelectedWeather()
    {
        return selectedWeather.load(std::memory_order_acquire);
    }

    void ReleaseOverride()
    {
        auto* sky = RE::Sky::GetSingleton();
        if (!sky || !sky->overrideWeather)
        {
            return;
        }
        sky->ReleaseWeatherOverride();
    }

    void SetEnabled(const bool a_enabled)
    {
        enabled.store(a_enabled, std::memory_order_release);
    }

    bool IsEnabled()
    {
        return enabled.load(std::memory_order_acquire);
    }

    bool Maintain(const bool a_force)
    {
        auto* locked = LockedWeather();
        auto* sky = RE::Sky::GetSingleton();
        if (!locked || !sky)
        {
            return false;
        }

        const auto releasePending = sky->flags.any(RE::Sky::Flags::kReleaseWeatherOverride);
        const auto stateChanged = sky->currentWeather != locked || sky->overrideWeather != locked || releasePending;
        if (!a_force && !stateChanged)
        {
            return false;
        }

        sky->flags.reset(RE::Sky::Flags::kReleaseWeatherOverride);
        if (stateChanged)
        {
            WeatherRuntime::SetWeatherInstant(locked, true);
        }
        else
        {
            sky->ForceWeather(locked, true);
        }
        return true;
    }
}  // namespace MPL::WeatherLock
