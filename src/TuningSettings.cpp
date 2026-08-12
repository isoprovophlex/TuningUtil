#include <DetailedLogging.h>
#include <LumaClient.h>
#include <SKSEMenuSettings.h>
#include <TuningSettings.h>
#include <mutex>

namespace MPL::TuningSettings
{
    namespace
    {
        std::mutex settingsLock;
        bool detailedLoggingConfigured = false;
        bool tuningMenuEnabledForSession = true;
        bool tuningMenuConfigured = true;
    }  // namespace

    void Load()
    {
        bool detailedLogging = false;
        LumaClient::GetProviderDetailedLogging(
            "TuningUtil",
            detailedLogging);
        const auto menuEnabled = SKSEMenuSettings::GetTuningMenuEnabled();
        {
            std::scoped_lock lock(settingsLock);
            detailedLoggingConfigured = detailedLogging;
            tuningMenuEnabledForSession = menuEnabled;
            tuningMenuConfigured = menuEnabled;
        }
        DetailedLogging::SetEnabled(detailedLogging);
        logger::info(
            "[TuningUtil] settings | tuningMenu={} | detailedLogging={} | startupOnly={}",
            menuEnabled,
            detailedLogging,
            !menuEnabled);
    }

    bool IsTuningMenuEnabledForSession()
    {
        std::scoped_lock lock(settingsLock);
        return tuningMenuEnabledForSession;
    }

    bool IsTuningMenuConfigured()
    {
        std::scoped_lock lock(settingsLock);
        return tuningMenuConfigured;
    }

    bool IsDetailedLoggingConfigured()
    {
        std::scoped_lock lock(settingsLock);
        return detailedLoggingConfigured;
    }

    bool SetTuningMenuConfigured(const bool a_enabled)
    {
        std::scoped_lock lock(settingsLock);
        const auto previous = tuningMenuConfigured;
        tuningMenuConfigured = a_enabled;
        if (SKSEMenuSettings::SetTuningMenuEnabled(a_enabled))
        {
            return true;
        }
        tuningMenuConfigured = previous;
        return false;
    }

    bool SetDetailedLoggingConfigured(const bool a_enabled)
    {
        {
            std::scoped_lock lock(settingsLock);
            const auto previous = detailedLoggingConfigured;
            detailedLoggingConfigured = a_enabled;
            if (!LumaClient::UpdateProviderDetailedLogging(
                    "TuningUtil",
                    a_enabled))
            {
                detailedLoggingConfigured = previous;
                return false;
            }
        }
        DetailedLogging::SetEnabled(a_enabled);
        return true;
    }
}  // namespace MPL::TuningSettings
