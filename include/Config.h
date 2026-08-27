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
        std::unordered_map<RE::TESWeather*, MPL::WeatherPatcher::WeatherBaseline> weatherBaselines;
        std::unordered_map<RE::BGSVolumetricLighting*, float> volumetricLightingIntensityBaselines;
        std::unordered_map<RE::BGSVolumetricLighting*, RE::NiColor> volumetricLightingColorBaselines;
        std::unordered_map<RE::TESImageSpace*, RE::ImageSpaceBaseData> imageSpaceBaselines;
        std::unordered_map<RE::BGSLightingTemplate*, MPL::LightingPatcher::Baseline> lightingTemplateBaselines;
        std::unordered_map<RE::TESObjectCELL*, MPL::LightingPatcher::Baseline> cellLightingBaselines;
    };
}
