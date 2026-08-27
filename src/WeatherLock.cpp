#include <DetailedLogging.h>
#include <WeatherLock.h>
#include <WeatherSyncClient.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace MPL::WeatherLock
{
    namespace
    {
        std::atomic<RE::TESWeather*> selectedWeather{ nullptr };
        std::atomic_bool enabled{ false };

        struct CallSite
        {
            std::uintptr_t address;
            bool isCall;
        };

        std::vector<CallSite> FindDirectReferences(const std::uintptr_t a_target)
        {
            std::vector<CallSite> result;
            const auto text = REL::Module::get().segment(REL::Segment::Name::textx);
            const auto begin = text.address();
            const auto end = begin + text.size();
            for (auto address = begin; address + 5 <= end; ++address)
            {
                const auto opcode = *reinterpret_cast<const std::uint8_t*>(address);
                if (opcode != 0xE8 && opcode != 0xE9)
                {
                    continue;
                }

                std::int32_t displacement = 0;
                std::memcpy(&displacement, reinterpret_cast<const void*>(address + 1), sizeof(displacement));
                const auto destination = address + 5 + displacement;
                if (destination == a_target)
                {
                    result.push_back({ address, opcode == 0xE8 });
                }
            }
            return result;
        }

        RE::TESWeather* LockedWeather()
        {
            return enabled.load(std::memory_order_acquire) ? selectedWeather.load(std::memory_order_acquire) : nullptr;
        }

        void SetWeatherThunk(RE::Sky* a_sky, RE::TESWeather* a_weather, bool a_override, bool a_accelerate)
        {
            auto* locked = LockedWeather();
            if (!a_sky || !locked)
            {
                if (a_sky)
                {
                    a_sky->SetWeather(a_weather, a_override, a_accelerate);
                }
                return;
            }

            if (a_weather != locked)
            {
                logger::debug(
                    "Blocked Sky::SetWeather request for {:08X}; locked weather is {:08X}",
                    a_weather ? a_weather->GetFormID() : 0,
                    locked->GetFormID());
                if (a_sky->currentWeather == locked && a_sky->overrideWeather == locked)
                {
                    return;
                }
                a_sky->ForceWeather(locked, true);
                return;
            }

            a_sky->SetWeather(locked, true, a_accelerate);
        }

        void ForceWeatherThunk(RE::Sky* a_sky, RE::TESWeather* a_weather, bool a_override)
        {
            auto* locked = LockedWeather();
            if (!a_sky || !locked)
            {
                if (a_sky)
                {
                    a_sky->ForceWeather(a_weather, a_override);
                }
                return;
            }

            if (a_weather != locked)
            {
                logger::debug(
                    "Blocked Sky::ForceWeather request for {:08X}; locked weather is {:08X}",
                    a_weather ? a_weather->GetFormID() : 0,
                    locked->GetFormID());
                if (a_sky->currentWeather == locked && a_sky->overrideWeather == locked)
                {
                    return;
                }
            }
            a_sky->ForceWeather(locked, true);
        }

        template <class Thunk>
        std::size_t InstallCallSiteHooks(const std::vector<CallSite>& a_sites, Thunk a_thunk)
        {
            if (a_sites.empty())
            {
                return 0;
            }

            SKSE::AllocTrampoline(a_sites.size() * 14);
            auto& trampoline = SKSE::GetTrampoline();
            for (const auto& site : a_sites)
            {
                if (site.isCall)
                {
                    trampoline.write_call<5>(site.address, a_thunk);
                }
                else
                {
                    trampoline.write_branch<5>(site.address, a_thunk);
                }
            }
            return a_sites.size();
        }
    }  // namespace

    void InstallHooks()
    {
        const REL::Relocation<std::uintptr_t> setWeatherTarget{ REL::RelocationID(25694, 26241) };
        const REL::Relocation<std::uintptr_t> forceWeatherTarget{ REL::RelocationID(25696, 26243) };
        const auto setWeatherSites = FindDirectReferences(setWeatherTarget.address());
        const auto forceWeatherSites = FindDirectReferences(forceWeatherTarget.address());
        const auto setWeatherHooks = InstallCallSiteHooks(setWeatherSites, SetWeatherThunk);
        const auto forceWeatherHooks = InstallCallSiteHooks(forceWeatherSites, ForceWeatherThunk);
        logger::info(
            "Installed weather-lock guards at {} Sky::SetWeather and {} Sky::ForceWeather engine call site(s)",
            setWeatherHooks,
            forceWeatherHooks);
        if (setWeatherHooks == 0)
        {
            logger::warn("Weather lock did not find any direct Sky::SetWeather engine call sites");
        }
    }

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
        const auto formID = sky->overrideWeather->GetFormID();
        sky->ReleaseWeatherOverride();
        DetailedLogging::Info(
            "Weather lock released weather override {:08X}",
            formID);
    }

    void SetEnabled(const bool a_enabled)
    {
        if (enabled.exchange(a_enabled, std::memory_order_acq_rel) == a_enabled)
        {
            return;
        }
        auto* weather = GetSelectedWeather();
        logger::info(
            "Weather lock {}{}",
            a_enabled ? "enabled" : "disabled",
            weather ? std::format(" for {:08X}", weather->GetFormID()) : " with no selected weather");
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
            DetailedLogging::Info(
                "Weather lock repaired sky state for {:08X}: current={:08X}, override={:08X}, releasePending={}",
                locked->GetFormID(),
                sky->currentWeather ? sky->currentWeather->GetFormID() : 0,
                sky->overrideWeather ? sky->overrideWeather->GetFormID() : 0,
                releasePending);
            const auto result =
                WeatherSyncClient::SetWeatherInstant(locked, true);
            DetailedLogging::Info(
                "Weather lock reapplied weather {:08X}; status={}, processed {} native cell emittance light-map entry/entries",
                locked->GetFormID(),
                static_cast<std::uint32_t>(result.status),
                result.lightCount);
        }
        else
        {
            sky->ForceWeather(locked, true);
        }
        return true;
    }
}  // namespace MPL::WeatherLock
