#include <DetailedLogging.h>
#include <LumaClient.h>
#include <TuningSettings.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>

namespace MPL::TuningSettings
{
    namespace
    {
        const std::filesystem::path kMenuSettingsPath{
            "./Data/Luma/Tuning/skseMenuSettings.json"
        };

        std::mutex settingsLock;
        bool detailedLoggingConfigured = false;
        bool tuningMenuEnabledForSession = true;
        bool tuningMenuConfigured = true;

        std::string ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            return file ?
                       std::string(
                           std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()) :
                       std::string{};
        }

        bool ReadTuningMenuEnabled()
        {
            const auto text = ReadText(kMenuSettingsPath);
            static const std::regex pattern(
                R"("enableTuningMenu"\s*:\s*(true|false))",
                std::regex::icase);
            std::smatch match;
            return !std::regex_search(text, match, pattern) ||
                   match[1].str() == "true" ||
                   match[1].str() == "TRUE";
        }

        bool WriteTuningMenuEnabledLocked(const bool a_enabled)
        {
            auto text = ReadText(kMenuSettingsPath);
            static const std::regex pattern(
                R"(("enableTuningMenu"\s*:\s*)(true|false))",
                std::regex::icase);
            std::smatch match;
            if (std::regex_search(text, match, pattern))
            {
                text.replace(
                    static_cast<std::size_t>(match.position(2)),
                    static_cast<std::size_t>(match.length(2)),
                    a_enabled ? "true" : "false");
            }
            else
            {
                const auto object = text.find('{');
                if (object == std::string::npos)
                {
                    logger::warn(
                        "Could not save the Tuning menu setting because {} is not a JSON object",
                        kMenuSettingsPath.string());
                    return false;
                }
                const auto content = text.find_first_not_of(
                    " \t\r\n",
                    object + 1);
                const bool empty =
                    content != std::string::npos && text[content] == '}';
                text.insert(
                    object + 1,
                    std::format(
                        "\n    \"enableTuningMenu\": {}{}",
                        a_enabled ? "true" : "false",
                        empty ? "\n" : ","));
            }

            std::ofstream file(
                kMenuSettingsPath,
                std::ios::binary | std::ios::trunc);
            file << text;
            if (!file)
            {
                logger::warn(
                    "Could not save the Tuning menu setting {}",
                    kMenuSettingsPath.string());
                return false;
            }
            return true;
        }

    }  // namespace

    void Load()
    {
        bool detailedLogging = false;
        bool notifications = false;
        LumaClient::GetProviderSettings(
            "TuningUtil",
            detailedLogging,
            notifications);
        const auto menuEnabled = ReadTuningMenuEnabled();
        {
            std::scoped_lock lock(settingsLock);
            detailedLoggingConfigured = detailedLogging;
            tuningMenuEnabledForSession = menuEnabled;
            tuningMenuConfigured = menuEnabled;
        }
        DetailedLogging::SetEnabled(detailedLogging);
        logger::info(
            "Tuning menu {}; detailed logging {}",
            menuEnabled ? "enabled" : "disabled (startup-only profile application)",
            detailedLogging ? "enabled" : "disabled");
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
        if (WriteTuningMenuEnabledLocked(a_enabled))
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
            if (!LumaClient::UpdateProviderSettings(
                    "TuningUtil",
                    a_enabled ? 1 : 0,
                    -1))
            {
                detailedLoggingConfigured = previous;
                return false;
            }
        }
        DetailedLogging::SetEnabled(a_enabled);
        return true;
    }
}  // namespace MPL::TuningSettings
