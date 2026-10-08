#include <DetailedLogging.h>
#include <FileIO.h>
#include <JsonOverlay.h>
#include <LumaClient.h>
#include <TuningSettings.h>
#include <fstream>
#include <mutex>
#include <optional>
#include <rfl/json.hpp>
#include <string>
#include <string_view>

namespace MPL::TuningSettings
{
    namespace
    {
        const std::filesystem::path kGlobalUserSettingsPath{ "./Data/SKSE/Plugins/Luma/globalUserSettings.json" };

        struct GlobalUserSettings
        {
            bool enableTuningMenu = true;
        };

        std::optional<std::string> ReadGlobalUserSettings(std::string& a_error)
        {
            std::error_code error;
            const auto exists = std::filesystem::exists(kGlobalUserSettingsPath, error);
            if (error)
            {
                a_error = error.message();
                return std::nullopt;
            }
            if (!exists) return "{}";
            std::ifstream file(kGlobalUserSettingsPath, std::ios::binary);
            if (!file)
            {
                a_error = "The global user settings file could not be opened.";
                return std::nullopt;
            }
            std::string text(std::istreambuf_iterator<char>(file), {});
            if (file.bad())
            {
                a_error = "The global user settings file could not be read.";
                return std::nullopt;
            }
            constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";
            if (text.starts_with(utf8Bom)) text.erase(0, utf8Bom.size());
            return text;
        }

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
        bool menuEnabled = true;
        std::string error;
        if (const auto text = ReadGlobalUserSettings(error))
        {
            const auto parsed = rfl::json::read<GlobalUserSettings, rfl::DefaultIfMissing>(*text);
            if (parsed) menuEnabled = parsed.value().enableTuningMenu;
            else error = parsed.error().what();
        }
        if (!error.empty())
            logger::warn("[TuningUtil] global user settings load failed | path={} | {}",
                kGlobalUserSettingsPath.string(), error);
        {
            std::scoped_lock lock(settingsLock);
            detailedLoggingConfigured = detailedLogging;
            tuningMenuEnabledForSession = menuEnabled;
            tuningMenuConfigured = menuEnabled;
        }
        DetailedLogging::SetEnabled(detailedLogging);
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
        std::string error;
        const auto text = ReadGlobalUserSettings(error);
        const auto updated = text ? JsonOverlay::SetBooleanMember(*text, "enableTuningMenu", a_enabled, error) : std::nullopt;
        std::error_code directoryError;
        if (updated)
            std::filesystem::create_directories(kGlobalUserSettingsPath.parent_path(), directoryError);
        if (directoryError) error = directoryError.message();
        if (!updated || directoryError || !FileIO::WriteAtomically(kGlobalUserSettingsPath, *updated, error))
        {
            logger::warn("[TuningUtil] global user settings save failed | path={} | {}",
                kGlobalUserSettingsPath.string(), error);
            return false;
        }
        tuningMenuConfigured = a_enabled;
        return true;
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
