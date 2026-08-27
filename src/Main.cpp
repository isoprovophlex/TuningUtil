#include <LumaAPI.h>
#include <LumaClient.h>
#include <PointLightPatcher.h>
#include <REL/Version.h>
#include <SKSE/API.h>
#include <TuningMenu.h>
#include <TuningSettings.h>
#include <TuningUtil.h>
#include <WeatherLock.h>
#include <WeatherSyncClient.h>

namespace
{
    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message) return;
        if (a_message->type == SKSE::MessagingInterface::kPostLoad)
        {
            MPL::TuningMenu::Register();
        }
        if (a_message->type == SKSE::MessagingInterface::kDataLoaded)
        {
            MPL::PointLightPatcher::InstallRuntimeEvents();
            if (!MPL::WeatherSyncClient::Load())
            {
                logger::warn(
                    "WeatherSync API is unavailable; weather selection will "
                    "use the engine fallback without an immediate emittance refresh");
            }
            MPL::TuningUtil::ApplyDataLoaded();
        }
    }
}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    if (!MPL::LumaClient::Load())
    {
        logger::critical("TuningUtil requires LumaUtil API version {}", MPL::LumaAPI::kVersion);
        return false;
    }
    MPL::TuningSettings::Load();
    logger::info("TuningUtil loaded for game version {}", a_skse->RuntimeVersion().string());
    if (MPL::TuningSettings::IsTuningMenuEnabledForSession())
    {
        MPL::WeatherLock::InstallHooks();
    }
    else
    {
        logger::info("Skipped weather-lock hooks because the Luma tuning menu is disabled for this launch");
    }
    SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
    MPL::LumaClient::SetRuntimeReady(true);
    return true;
}

SKSEPluginInfo(
        .Version = REL::Version{ 0, 1, 0, 0 },
    .Name = "TuningUtil"sv,
    .Author = "isoprovophlex"sv,
    .SupportEmail = ""sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)
