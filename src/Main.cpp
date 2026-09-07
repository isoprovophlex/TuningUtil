#include <LumaServiceAPI.h>
#include <LumaClient.h>
#include <ObjectLightingPatcher.h>
#include <PointLightPatcher.h>
#include <REL/Version.h>
#include <SKSE/API.h>
#include <TuningMenu.h>
#include <TuningSettings.h>
#include <TuningUtil.h>
#include <WeatherLock.h>

namespace
{
    bool runtimeInitialized = false;

    void InitializeRuntime()
    {
        if (runtimeInitialized || !MPL::LumaClient::Load()) return;

        MPL::TuningSettings::Load();
        if (MPL::TuningSettings::IsTuningMenuEnabledForSession())
        {
            MPL::WeatherLock::InstallHooks();
        }
        MPL::LumaClient::SetRuntimeReady(true);
        runtimeInitialized = true;
    }

    void OnMMSFMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (a_message && a_message->type == MPL::API::MMSF::MMSFMessage::kMessage_MMSFServicesReg)
        {
            InitializeRuntime();
        }
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message)
        {
            return;
        }
        if (a_message->type == SKSE::MessagingInterface::kPostPostLoad)
        {
            // Subscribe after Luma's PostLoad listener, then handle an already registered service.
            SKSE::GetMessagingInterface()->RegisterListener("MMSF", OnMMSFMessage);
            InitializeRuntime();
            return;
        }
        if (!runtimeInitialized)
        {
            if (a_message->type == SKSE::MessagingInterface::kDataLoaded)
            {
                logger::critical("[TuningUtil] startup failed | MMSF API=2 and LUMA service={} required", MPL::API::Luma::kVersion);
            }
            return;
        }
        switch (a_message->type)
        {
        case SKSE::MessagingInterface::kDataLoaded:
            {
                MPL::PointLightPatcher::InstallRuntimeEvents();
                MPL::ObjectLightingPatcher::InstallRuntimeEvents();
                MPL::TuningUtil::ApplyDataLoaded();
                MPL::TuningMenu::Register();
                break;
            }
        case SKSE::MessagingInterface::kPreLoadGame:
            MPL::PointLightPatcher::ResetCellTracking();
            MPL::ObjectLightingPatcher::ResetReferenceTracking();
            break;
        case SKSE::MessagingInterface::kNewGame:
            MPL::PointLightPatcher::ResetCellTracking();
            MPL::ObjectLightingPatcher::ResetReferenceTracking();
            MPL::ObjectLightingPatcher::QueueLoadedReferenceRefresh();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            MPL::ObjectLightingPatcher::QueueLoadedReferenceRefresh();
            break;
        default:
            break;
        }
    }
}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    logger::info("[TuningUtil] startup | game={}", a_skse->RuntimeVersion().string());
    const auto* messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener(OnSKSEMessage);
}

SKSEPluginInfo(
        .Version = REL::Version{ 0, 1, 0, 0 },
    .Name = "TuningUtil"sv,
    .Author = "isoprovophlex"sv,
    .SupportEmail = ""sv,
    .StructCompatibility = SKSE::StructCompatibility::Independent,
    .RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)
