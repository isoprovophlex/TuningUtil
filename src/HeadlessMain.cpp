#include <Config.h>
#ifdef TUNING_ATLM
#include <CSTonemapping.h>
#include <HeliosphanClient.h>
#include <LightingPatcher.h>
#else
#include <WeatherPatcher.h>
#endif
#include <DetailedLogging.h>
#include <HeadlessProfile.h>
#ifdef TUNING_HELIOS
#include <HeadlessLogging.h>
#endif
#include <ImageSpacePatcher.h>
#include <LumaServiceAPI.h>
#include <MMSFProviderSettings.h>
#include <TuningUtil.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

namespace
{
#ifdef TUNING_ATLM
    constexpr std::string_view provider = "ATLM";
#else
    constexpr std::string_view provider = "Helios";
#endif
    constexpr std::string_view pluginName = TUNING_PLUGIN_NAME;
    constexpr std::size_t startupLogCapacity = 128;
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> startupLogs;
    bool initialized = false;

    void ActivateLogging()
    {
#ifdef TUNING_HELIOS
        MPL::API::Heliosphan::LogMessage write = nullptr;
        if (const auto module = GetModuleHandleW(L"Heliosphan.dll"))
        {
            const auto query = reinterpret_cast<MPL::API::Heliosphan::GetLoggingAPI>(
                GetProcAddress(module, MPL::API::Heliosphan::kLoggingAPIExport));
            if (query) write = query(MPL::API::Heliosphan::kLoggingAPIVersion);
        }
        const auto directory = SKSE::log::log_directory();
        if (write && directory)
        {
            auto sink = std::make_shared<MPL::HeadlessLogging::HeliosphanSink>(
                write, *directory / "HeliosTuning.log");
            auto log = std::make_shared<spdlog::logger>(std::string(pluginName), std::move(sink));
            log->set_level(spdlog::level::info);
            log->flush_on(spdlog::level::info);
            spdlog::set_default_logger(std::move(log));
        }
        else
        {
            SKSE::log::init();
        }
#else
        SKSE::log::init();
#endif
        for (const auto& message : startupLogs->last_raw())
        {
            spdlog::default_logger()->log(message.time, message.source, message.level, message.payload);
        }
        startupLogs.reset();
#ifdef TUNING_HELIOS
        if (!write || !directory)
        {
            logger::warn("[HeliosTuning] Heliosphan logging API unavailable; using HeliosTuning.log");
        }
#endif
    }

    bool FullRuntimeLoaded()
    {
        const auto module = GetModuleHandleW(L"TuningUtil.dll");
        if (!module) return false;
        using IsLoaded = bool (*)();
        const auto isLoaded = reinterpret_cast<IsLoaded>(GetProcAddress(module, "TuningUtil_IsLoaded"));
        // Older full builds have no marker, so keep them as the owner.
        return !isLoaded || isLoaded();
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message || a_message->type != SKSE::MessagingInterface::kDataLoaded || initialized) return;
        initialized = true;
        if (FullRuntimeLoaded())
        {
            spdlog::set_default_logger(std::make_shared<spdlog::logger>(
                std::string(pluginName), std::make_shared<spdlog::sinks::null_sink_mt>()));
            startupLogs.reset();
            return;
        }
        ActivateLogging();
        auto* stat = MPL::Config::StatData::GetSingleton();
        auto* registry = stat->GetMMSFAPI();
        auto* service = registry ? registry->QueryService("LUMA") : nullptr;
        if (!service || service->GetVersion() != MPL::API::Luma::kVersion || !stat->GetEDIDCache())
        {
            logger::error("[{}] inactive | MMSF API=2, EDID service=1 and LUMA service={} required",
                pluginName, MPL::API::Luma::kVersion);
            return;
        }
        bool detailedLogging = false;
        MPL::MMSFProviderSettings::ReadDetailedLogging(registry, provider.data(), detailedLogging);
        MPL::DetailedLogging::SetEnabled(detailedLogging);
#ifdef TUNING_ATLM
        MPL::HeliosphanClient::Initialize(registry);
#endif
        if (!MPL::TuningUtil::LoadHeadlessProfile(provider)) return;
        auto profileName = std::string(provider);
        if (!MPL::TuningUtil::GetSettings(profileName).EnableProfile)
        {
            logger::info("[{}] inactive | profile disabled", pluginName);
            return;
        }
#ifdef TUNING_ATLM
        MPL::LightingPatcher::ApplyDataLoaded();
        MPL::CSTonemapping::Initialize();
#else
        MPL::WeatherPatcher::ApplyDataLoaded();
#endif
        MPL::ImageSpacePatcher::ApplyFilmicCurveWhitePoint();
        logger::info("[{}] startup complete | headless | provider={}", pluginName, provider);
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    startupLogs = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(startupLogCapacity);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(std::string(pluginName), startupLogs));
    SKSE::Init(a_skse, false);
    logger::info("[{}] startup | game={}", pluginName, a_skse->RuntimeVersion().string());
    const auto* messaging = SKSE::GetMessagingInterface();
    return messaging && messaging->RegisterListener(OnSKSEMessage);
}

SKSEPluginVersion = []() constexpr
{
    SKSE::PluginVersionData version;
    version.PluginVersion(REL::Version{ 0, 1, 0, 0 });
    version.PluginName(TUNING_PLUGIN_NAME);
    version.AuthorName("isoprovophlex"sv);
    version.UsesNoStructs();
    version.UsesAddressLibrary();
    static_assert((SKSE::PluginVersionData{}.versionIndependenceEx &
        SKSE::PluginVersionData::kVersionIndependentEx_AddressLibraryV5) != 0);
    return version;
}();

SKSE_EXPORT bool SKSEPlugin_Query(SKSE::QueryInterface*, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = SKSEPlugin_Version.GetPluginName().data();
    a_info->version = SKSEPlugin_Version.GetPluginVersion().pack();
    return true;
}
