#pragma once

#include <Config/Common.h>
#include <Config/Lighting.h>
#include <Config/Tuning.h>
#include <Config/Weathers.h>
#include <MMSF_API.h>

namespace MPL::Config
{
    class StatData : public REX::Singleton<StatData>
    {
    public:
        MPL::API::MMSF::Interface* mmsfAPI = nullptr;
        MPL::API::MMSF::IEDIDCache* edidCache = nullptr;

        API::MMSF::IEDIDCache* GetEDIDCache()
        {
            if (edidCache) return edidCache;
            if (!mmsfAPI) mmsfAPI = API::MMSF::RequestMMSFAPI();
            if (!mmsfAPI) return nullptr;

            constexpr std::uint8_t supportedAPIVersion = 2;
            constexpr std::uint8_t supportedCacheVersion = 1;
            const auto features = mmsfAPI->GetVersion();
            if (API::MMSF::GetVersion(features) != supportedAPIVersion ||
                (features & API::MMSF::MMSFAPIFeatures::kCoreService) != API::MMSF::MMSFAPIFeatures::kCoreService)
                return nullptr;

            auto* service = mmsfAPI->QueryService("EDID");
            if (!service || service->GetVersion() != supportedCacheVersion) return nullptr;
            edidCache = static_cast<API::MMSF::IEDIDCache*>(service);
            return edidCache;
        }

        std::unordered_map<RE::TESWeather*, MPL::WeatherPatcher::WeatherBaseline> weatherBaselines;
        std::unordered_map<RE::BGSVolumetricLighting*, float> volumetricLightingIntensityBaselines;
        std::unordered_map<RE::BGSVolumetricLighting*, RE::NiColor> volumetricLightingColorBaselines;
        std::unordered_map<RE::TESImageSpace*, RE::ImageSpaceBaseData> imageSpaceBaselines;
        std::unordered_map<RE::BGSLightingTemplate*, MPL::LightingPatcher::Baseline> lightingTemplateBaselines;
        std::unordered_map<RE::TESObjectCELL*, MPL::LightingPatcher::Baseline> cellLightingBaselines;
    };
}
