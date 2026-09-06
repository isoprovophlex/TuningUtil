#include <DetailedLogging.h>
#include <WeatherRuntime.h>

#include <algorithm>
#include <limits>

namespace MPL::WeatherRuntime
{
    namespace
    {
        using UpdateCellEmittance = void (*)(RE::TESObjectCELL*);

        REL::Relocation<UpdateCellEmittance> updateCellEmittance{
            REL::VariantID(18464, 18895, 0x272820)
        };
    }  // namespace

    std::uint32_t RefreshCurrentCellEmittance()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        auto* loadedData = cell ? cell->GetRuntimeData().loadedData : nullptr;
        if (!loadedData)
        {
            return 0;
        }

        std::uint32_t refreshedRegions = 0;
        for (const auto& entry : loadedData->emittanceSourceRefMap)
        {
            auto* source = entry.first;
            if (!source || !source->Is(RE::FormType::Region))
            {
                continue;
            }

            auto* region = static_cast<RE::TESRegion*>(source);
            if (auto* weather = region->currentWeather)
            {
                region->SetCurrentWeather(weather);
                ++refreshedRegions;
            }
        }

        const auto lightEntries = loadedData->emittanceLightRefMap.size();
        updateCellEmittance(cell);
        DetailedLogging::Info(
            "[Effect Lighting] refresh | cell={:08X} | regions={} | lights={}",
            cell->GetFormID(),
            refreshedRegions,
            lightEntries);
        return static_cast<std::uint32_t>(
            std::min<std::size_t>(
                lightEntries,
                std::numeric_limits<std::uint32_t>::max()));
    }

    SetWeatherResult SetWeatherInstant(
        RE::TESWeather* a_weather,
        const bool a_override)
    {
        SetWeatherResult result{};
        if (!a_weather)
        {
            return result;
        }
        auto* sky = RE::Sky::GetSingleton();
        if (!sky)
        {
            result.status = SetWeatherStatus::kSkyUnavailable;
            return result;
        }

        sky->ForceWeather(a_weather, a_override);

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell)
        {
            result.status = SetWeatherStatus::kAppliedWithoutPlayerCell;
            return result;
        }
        auto* loadedData = cell->GetRuntimeData().loadedData;
        if (!loadedData)
        {
            result.status = SetWeatherStatus::kAppliedWithoutLoadedData;
            return result;
        }

        for (const auto& entry : loadedData->emittanceSourceRefMap)
        {
            auto* source = entry.first;
            if (!source || !source->Is(RE::FormType::Region))
            {
                continue;
            }

            auto* region = static_cast<RE::TESRegion*>(source);
            region->SetCurrentWeather(a_weather);
        }
        const auto lightEntries =
            loadedData->emittanceLightRefMap.size();
        updateCellEmittance(cell);
        result.status = SetWeatherStatus::kAppliedAndRefreshed;
        result.lightCount = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                lightEntries,
                std::numeric_limits<std::uint32_t>::max()));
        return result;
    }
}  // namespace MPL::WeatherRuntime
