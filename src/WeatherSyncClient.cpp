#include <WeatherSyncAPI.h>
#include <WeatherSyncClient.h>

namespace MPL::WeatherSyncClient
{
    namespace
    {
        HMODULE module = nullptr;
        const WeatherSyncAPI::Interface* api = nullptr;
    }  // namespace

    bool Load()
    {
        module = GetModuleHandleW(L"WeatherSync.dll");
        const auto request =
            module ?
                reinterpret_cast<WeatherSyncAPI::RequestInterface>(
                    GetProcAddress(module, "WeatherSync_RequestAPI")) :
                nullptr;
        api = request ? request(WeatherSyncAPI::kVersion) : nullptr;
        return api && api->version == WeatherSyncAPI::kVersion &&
               api->SetWeatherInstant;
    }

    WeatherSyncAPI::SetWeatherResult SetWeatherInstant(
        RE::TESWeather* a_weather,
        const bool a_override)
    {
        if (api && api->SetWeatherInstant)
        {
            return api->SetWeatherInstant(a_weather, a_override);
        }
        WeatherSyncAPI::SetWeatherResult result{};
        auto* sky = RE::Sky::GetSingleton();
        if (!a_weather)
        {
            return result;
        }
        if (!sky)
        {
            result.status =
                WeatherSyncAPI::SetWeatherStatus::kSkyUnavailable;
            return result;
        }
        sky->ForceWeather(a_weather, a_override);
        result.status = WeatherSyncAPI::SetWeatherStatus::
            kAppliedWithoutLoadedData;
        result.flags |= WeatherSyncAPI::ToMask(
            WeatherSyncAPI::SetWeatherFlag::kWeatherApplied);
        return result;
    }
}  // namespace MPL::WeatherSyncClient
