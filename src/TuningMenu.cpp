#include <Config.h>
#include <ImageSpacePatcher.h>
#include <JsonOverlay.h>
#include <LightingPatcher.h>
#include <RecordFilter.h>
#include <RE/M/Main.h>
#include <RE/U/UIBlurManager.h>
#include <RegionRuntime.h>
#include <SKSEMenuFramework.h>
#include <SKSEMenuSettings.h>
#include <SliderCreator.h>
#include <SliderSettingCatalog.h>
#include <TuningMenu.h>
#include <TuningSettings.h>
#include <TuningUtil.h>
#include <WeatherLock.h>
#include <WeatherPatcher.h>
#include <WeatherRuntime.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace MPL::TuningMenu
{
    namespace
    {
        using namespace std::chrono_literals;

        const std::filesystem::path kProfileRoot{ "./Data/Luma/Tuning" };
        const std::filesystem::path kQuickSelectRoot{ "./Data/SKSE/Plugins/Luma" };
        const std::filesystem::path kMenuFrameworkIni{ "./Data/SKSE/Plugins/SKSEMenuFramework.ini" };
        constexpr std::string_view kMenuDefinitionFileName = "skseMenu.json";
        constexpr std::string_view kQuickSelectListFileName = "quickSelectList.json";
        constexpr float kQuickSelectWindowLength = 210.0f;
        constexpr float kWeatherSelectWindowLength = kQuickSelectWindowLength * 3.0f;

        struct MenuEffectOverride
        {
            bool freezeTimeEnabled = true;
            bool backgroundBlurEnabled = true;
            bool lumaRenderedThisFrame = false;
            bool active = false;
            bool blurContributionRemoved = false;
            std::atomic_bool openInitializationPending{ false };
            std::atomic_bool closeCleanupPending{ false };
        };

        struct ButtonFeedbackStyle
        {
            ButtonFeedbackStyle()
            {
                const auto brighten = [](const ImGuiMCP::ImVec4& a_color, const float a_brightness)
                {
                    const auto channel = [&](const float a_value)
                    {
                        const auto value = std::clamp(a_value, 0.0f, 1.0f);
                        return value + ((1.0f - value) * a_brightness);
                    };
                    return ImGuiMCP::ImVec4(
                        channel(a_color.x), channel(a_color.y), channel(a_color.z), a_color.w);
                };
                const auto* base = ImGuiMCP::GetStyleColorVec4(ImGuiMCP::ImGuiCol_Button);
                const auto button = base ? *base : ImGuiMCP::ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
                ImGuiMCP::PushStyleColor(
                    ImGuiMCP::ImGuiCol_ButtonHovered,
                    brighten(button, SKSEMenuSettings::GetHoverBrightness()));
                ImGuiMCP::PushStyleColor(
                    ImGuiMCP::ImGuiCol_ButtonActive,
                    brighten(button, SKSEMenuSettings::GetPressedBrightness()));
            }

            ~ButtonFeedbackStyle()
            {
                ImGuiMCP::PopStyleColor(2);
            }

            ButtonFeedbackStyle(const ButtonFeedbackStyle&) = delete;
            ButtonFeedbackStyle& operator=(const ButtonFeedbackStyle&) = delete;
        };

        struct ButtonColorStyle
        {
            explicit ButtonColorStyle(const std::optional<std::array<float, 4>>& a_color)
            {
                if (!a_color)
                {
                    return;
                }

                const auto makeColor = [&](const float a_brightness)
                {
                    const auto brighten = [&](const float a_channel)
                    {
                        const auto channel = std::clamp(a_channel, 0.0f, 1.0f);
                        return channel + ((1.0f - channel) * a_brightness);
                    };
                    return ImGuiMCP::ImVec4(
                        brighten((*a_color)[0]),
                        brighten((*a_color)[1]),
                        brighten((*a_color)[2]),
                        std::clamp((*a_color)[3], 0.0f, 1.0f));
                };

                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, makeColor(0.0f));
                ImGuiMCP::PushStyleColor(
                    ImGuiMCP::ImGuiCol_ButtonHovered,
                    makeColor(SKSEMenuSettings::GetHoverBrightness()));
                ImGuiMCP::PushStyleColor(
                    ImGuiMCP::ImGuiCol_ButtonActive,
                    makeColor(SKSEMenuSettings::GetPressedBrightness()));
                applied = true;
            }

            ~ButtonColorStyle()
            {
                if (applied)
                {
                    ImGuiMCP::PopStyleColor(3);
                }
            }

            ButtonColorStyle(const ButtonColorStyle&) = delete;
            ButtonColorStyle& operator=(const ButtonColorStyle&) = delete;

        private:
            bool applied = false;
        };

        struct TimedMessage
        {
            TimedMessage& operator=(std::string a_value)
            {
                text = std::move(a_value);
                changedAt = std::chrono::steady_clock::now();
                return *this;
            }

            bool empty() const { return text.empty(); }
            const char* c_str() const { return text.c_str(); }
            void clear() { text.clear(); }

            std::string text;
            std::chrono::steady_clock::time_point changedAt{};
        };

        struct MenuControl
        {
            std::string type;
            std::string id;
            std::string label;
            std::string header;
            std::optional<std::string> displayName;
            std::string setting;
            struct SliderTarget
            {
                std::string setting;
                double scale = 1.0;
                bool ignoreLink = false;
            };

            using SliderTargetValue = std::variant<std::string, SliderTarget>;

            std::vector<SliderTargetValue> settings;
            std::string link;
            std::string tooltip;
            float min = std::numeric_limits<float>::quiet_NaN();
            float max = std::numeric_limits<float>::quiet_NaN();
            float step = std::numeric_limits<float>::quiet_NaN();
            float width = std::numeric_limits<float>::quiet_NaN();
            std::string format;
            float fontScale = 1.0f;
            std::optional<std::array<float, 4>> color;
            bool invert = false;
            bool defaultOpen = false;
            bool advanced = false;
        };

        std::string_view SliderTargetPath(const MenuControl::SliderTargetValue& a_target)
        {
            return std::visit(
                [](const auto& a_value) -> std::string_view
                {
                    using Value = std::decay_t<decltype(a_value)>;
                    if constexpr (std::same_as<Value, std::string>) return a_value;
                    else return a_value.setting;
                },
                a_target);
        }

        bool SliderTargetIgnoresLink(const MenuControl::SliderTargetValue& a_target)
        {
            const auto* target = std::get_if<MenuControl::SliderTarget>(&a_target);
            return target && target->ignoreLink;
        }

        bool IsInteriorLinkableSliderSetting(const std::string_view a_setting)
        {
            return a_setting.starts_with("intBrightnessMultiplier.") ||
                   a_setting.starts_with("intSaturationMultiplier.") ||
                   a_setting.starts_with("intHueShift.");
        }

        struct MenuPage
        {
            std::string title;
            std::string description;
            std::size_t order = 0;
            bool advanced = false;
            std::vector<MenuControl> modules;
        };

        struct MenuDefinition
        {
            int schemaVersion = 0;
            std::string profile;
            std::string title;
            std::string description;
            bool enabled = true;
            bool lockEditMode = false;
            MenuPage profilePage;
            std::vector<MenuPage> pages;
        };

        struct LoadedMenu
        {
            std::filesystem::path path;
            MenuDefinition definition;
        };

        struct DefinitionFile
        {
            std::filesystem::path path;
            std::filesystem::file_time_type writeTime;

            bool operator==(const DefinitionFile&) const = default;
        };

        struct WeatherMenuEntry
        {
            RE::TESWeather* weather = nullptr;
            std::string label;
        };

        struct RecordMenuEntry
        {
            RE::TESForm* form = nullptr;
            std::string label;
        };

        struct QuickSelectList
        {
            std::vector<std::string> weathers;
        };

        struct PresetSaveInput
        {
            std::array<char, 96> category{};
            std::array<char, 128> name{};
            std::string removalCategory;
            std::string removalPreset;
            std::array<char, 96> categoryRename{};
            std::array<char, 128> presetRename{};
            std::string categoryRenameSource;
            std::string presetRenameSource;
            std::string settingsSelection;
            std::vector<JsonOverlay::ValueEntry> settings;
            std::string settingsError;
        };

        struct PendingPresetRemovals
        {
            std::unordered_map<std::string, std::string> categories;
            std::unordered_map<std::string, WeatherPatcher::ActivePreset> presets;
        };

        struct PresetVisualState
        {
            std::uint64_t settingsRevision = 0;
            std::string activeSignature;
            std::unordered_set<std::string> differingPresets;
            bool initialized = false;
        };

        struct LayoutEditorState
        {
            struct ModuleNameInput
            {
                std::array<char, 256> value{};
                std::string source;
            };

            std::array<char, 128> newPageName{};
            std::array<char, 128> pageName{};
            std::array<char, 256> elementText{};
            std::string pageNameSource;
            std::unordered_map<std::string, ModuleNameInput> moduleNames;
            int elementChoice = 0;
            int moduleChoice = 0;
            bool moduleAdvanced = false;
        };

        struct LayoutEditSession
        {
            std::filesystem::path sourcePath;
            std::filesystem::path workingPath;
            std::vector<std::optional<std::size_t>> pageOrigins;
            bool dirty = false;
        };

        enum class SliderCreatorDomain
        {
            weather,
            interior,
        };

        struct SliderCreatorState
        {
            std::string profile;
            SliderCreatorDomain domain = SliderCreatorDomain::weather;
            std::size_t pageIndex = 0;
            std::optional<std::size_t> loadedPageIndex;
            std::optional<std::size_t> loadedControlIndex;
            std::string loadedSliderID;
            std::array<char, 128> label{};
            std::array<char, 256> tooltip{};
            std::array<char, 64> link{};
            std::array<char, 64> localLink{};
            std::array<char, 32> format{};
            std::array<char, 96> includeContainsInput{};
            std::array<char, 96> excludeContainsInput{};
            int includeContainsSelection = -1;
            int excludeContainsSelection = -1;
            std::vector<SliderCreator::Target> settings;
            SliderCreator::Filter include;
            SliderCreator::Filter exclude;
            RE::TESWeather* selectedWeather = nullptr;
            RE::BGSLightingTemplate* selectedLightingTemplate = nullptr;
            RE::TESObjectLIGH* selectedBaseLight = nullptr;
            int catalogGroup = 0;
            int catalogSetting = 0;
            float pendingScale = 1.0f;
            bool pendingIgnoreLink = false;
            bool useHueScales = false;
            std::array<float, 7> hueScales{ 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
            bool filtered = true;
            bool invert = false;
            bool useTimes = false;
            std::array<bool, 4> times{ true, true, true, true };
            bool useDefault = false;
            float defaultValue = 1.0f;
            bool useRange = false;
            float minimum = 0.0f;
            float maximum = 2.0f;
            float step = 0.1f;
            bool useWidth = false;
            float width = 0.0f;
            float functionalPreviewValue = std::numeric_limits<float>::quiet_NaN();
            std::string functionalPreviewKey;
            bool initialized = false;
        };

        struct TextListEditorState
        {
            std::array<char, 96> input{};
            int selection = -1;
        };

        struct PluginFilterEditorState
        {
            TextListEditorState includeExact;
            TextListEditorState includeContains;
            TextListEditorState excludeExact;
            TextListEditorState excludeContains;
        };

        struct WeatherFilterEditorState
        {
            std::array<char, 96> includeContainsInput{};
            std::array<char, 96> excludeContainsInput{};
            int includeContainsSelection = -1;
            int excludeContainsSelection = -1;
            RE::TESWeather* selectedWeather = nullptr;
        };

        struct RecordFilterEditorState
        {
            std::array<char, 96> includeContainsInput{};
            std::array<char, 96> excludeContainsInput{};
            int includeContainsSelection = -1;
            int excludeContainsSelection = -1;
            RE::TESForm* selectedRecord = nullptr;
            PluginFilterEditorState plugins;
        };

        struct DynamicAmbientModuleState
        {
            WeatherPatcher::DynamicAmbientRange range;
            std::optional<WeatherPatcher::DynamicBrightnessStatus> status;
            std::uint64_t settingsRevision = std::numeric_limits<std::uint64_t>::max();
        };

        enum class RecordFilterKind
        {
            lightingTemplate,
            baseLight,
            effectLighting,
        };

        struct NamedLinkable
        {
            std::string_view key;
            std::string_view label;
            WeatherPatcher::SettingLink* value;
            double* direct = nullptr;
        };

        struct NamedValue
        {
            std::string_view key;
            std::string_view label;
            double* value;
        };

        struct NamedHueShift
        {
            std::string_view key;
            std::string_view label;
            WeatherPatcher::SettingLink* value;
            WeatherPatcher::HueShiftBands* direct;
        };

        struct SliderSetting
        {
            double resolved = 0.0;
            WeatherPatcher::SettingLink* link = nullptr;
            double* scalar = nullptr;
            WeatherPatcher::HueShiftBands* hueShift = nullptr;
            WeatherPatcher::HueShiftBands resolvedHueShift{};
            double WeatherPatcher::HueShiftBands::* hueBand = nullptr;

            void Set(const double a_value) const
            {
                if (scalar) *scalar = a_value;
                else if (hueShift && hueBand)
                {
                    auto direct = resolvedHueShift;
                    direct.*hueBand = a_value;
                    *hueShift = direct;
                }
            }

            void ResolveWithoutLink()
            {
                if (!link) return;
                if (scalar) resolved = *scalar;
                else if (hueShift && hueBand) resolved = (*hueShift).*hueBand;
            }
        };

        std::optional<SliderSetting> FindSliderSetting(TuningUtil::Settings&, std::string_view);
        bool DrawHueRanges(WeatherPatcher::HueRanges&, const std::string&);
        bool AddUniqueString(std::vector<std::string>&, std::string);
        void DrawCreatorContainsList(std::vector<std::string>&, std::array<char, 96>&, int&, const std::string&);
        void DrawCreatorPluginContainsList(std::vector<std::string>&, std::array<char, 96>&, int&, const std::string&);
        void DrawCreatorPluginList(std::vector<std::string>&, std::array<char, 96>&, int&, const std::string&);
        void DrawCreatorWeatherList(std::vector<std::string>&, const std::string&);
        void DrawCreatorRecordList(std::vector<std::string>&, const std::string&, RecordFilterKind);
        void RefreshProfileMenuState(const MenuDefinition&, std::span<const std::string>);
        std::string LayoutModuleDisplayName(const MenuControl&);
        void QueueLayoutEditReload(LayoutEditSession&, std::string);
        void DrawItemTooltip(const std::string&);
        template <std::size_t Size>
        void SetInputText(std::array<char, Size>&, std::string_view);

        std::vector<LoadedMenu> profileMenus;
        std::vector<DefinitionFile> loadedDefinitionFiles;
        std::vector<std::filesystem::path> registeredProfilePaths;
        std::chrono::steady_clock::time_point nextDefinitionCheck{};
        TimedMessage statusMessage;
        TimedMessage settingsStatusMessage;
        std::optional<std::string> pendingMenuReloadStatus;
        std::unordered_map<std::string, std::vector<WeatherMenuEntry>> weatherMenuEntries;
        std::unordered_map<std::string, std::vector<RE::TESWeather*>> quickWeatherSelections;
        std::unordered_map<std::string, PresetSaveInput> presetSaveInputs;
        std::unordered_map<std::string, PendingPresetRemovals> pendingPresetRemovals;
        std::unordered_map<std::string, SliderCreatorState> sliderCreatorStates;
        std::unordered_map<std::string, PluginFilterEditorState> pluginFilterEditorStates;
        std::unordered_map<std::string, WeatherFilterEditorState> weatherFilterEditorStates;
        std::unordered_map<std::string, RecordFilterEditorState> recordFilterEditorStates;
        std::unordered_map<std::string, DynamicAmbientModuleState> dynamicAmbientModuleStates;
        std::unordered_map<std::string, std::vector<WeatherMenuEntry>> sliderCreatorWeatherEntries;
        std::optional<std::vector<RecordMenuEntry>> lightingTemplateMenuEntries;
        std::optional<std::vector<RecordMenuEntry>> baseLightMenuEntries;
        std::optional<std::vector<RecordMenuEntry>> effectLightingMenuEntries;
        std::unordered_map<std::string, PresetVisualState> presetVisualStates;
        std::unordered_map<std::string, int> profilePriorityInputs;
        std::unordered_map<std::string, bool> weatherLockPreferences;
        std::unordered_map<std::string, LayoutEditorState> layoutEditorStates;
        std::unordered_map<std::string, LayoutEditSession> layoutEditSessions;
        std::unordered_map<std::string, std::string> activeProfilePages;
        std::unordered_map<std::string, std::size_t> requestedProfilePageIndices;
        std::unordered_set<std::string> requestedAutomaticProfilePages;
        std::array<char, 96> newProfileName{};
        std::string profileCopySource;
        std::string activeWeatherLockProfile;
        std::string activeMenuPage;
        std::atomic_bool menuFrameworkOpen{ false };
        bool editModeEnabled = false;
        bool lightPlacerCommitPending = false;
        MenuEffectOverride menuEffectOverride;
        SKSEMenuFramework::Model::Event* menuFrameworkEvent = nullptr;
        bool registered = false;

        std::filesystem::path LayoutWorkingPath(const std::filesystem::path& a_source)
        {
            return a_source.parent_path() / "skseMenu.edit.json";
        }

        std::filesystem::path LayoutCommitPath(const std::filesystem::path& a_source)
        {
            return a_source.parent_path() / "skseMenu.commit.json";
        }

        LayoutEditSession* EnsureLayoutEditSession(
            const std::string& a_profile,
            const std::filesystem::path& a_source,
            std::string& a_error)
        {
            a_error.clear();
            auto [entry, inserted] = layoutEditSessions.try_emplace(
                a_profile,
                LayoutEditSession{ a_source, LayoutWorkingPath(a_source) });
            auto& session = entry->second;
            std::error_code existsError;
            if (!inserted && std::filesystem::is_regular_file(session.workingPath, existsError) && !existsError)
            {
                return std::addressof(session);
            }

            std::error_code cleanupError;
            std::filesystem::remove(session.workingPath, cleanupError);
            std::filesystem::remove(LayoutCommitPath(a_source), cleanupError);
            std::error_code copyError;
            std::filesystem::copy_file(
                a_source,
                session.workingPath,
                std::filesystem::copy_options::overwrite_existing,
                copyError);
            if (copyError)
            {
                layoutEditSessions.erase(entry);
                a_error = std::format("Could not begin the Edit Mode session: {}", copyError.message());
                return nullptr;
            }
            std::string loadError;
            const auto pages = SliderCreator::Load(a_source, loadError);
            if (!loadError.empty())
            {
                std::error_code removeError;
                std::filesystem::remove(session.workingPath, removeError);
                layoutEditSessions.erase(entry);
                a_error = std::format("Could not read the menu pages for Edit Mode: {}", loadError);
                return nullptr;
            }
            session.pageOrigins.reserve(pages.size());
            for (std::size_t index = 0; index < pages.size(); ++index) session.pageOrigins.emplace_back(index);
            session.dirty = false;
            return std::addressof(session);
        }

        std::filesystem::path ActiveLayoutPath(
            const std::string_view a_profile,
            const std::filesystem::path& a_source)
        {
            const auto session = layoutEditSessions.find(std::string(a_profile));
            return session != layoutEditSessions.end() ? session->second.workingPath : a_source;
        }

        bool SaveLayoutEditSession(LayoutEditSession& a_session, std::string& a_error)
        {
            a_error.clear();
            const auto commitPath = LayoutCommitPath(a_session.sourcePath);
            std::error_code copyError;
            std::filesystem::copy_file(
                a_session.workingPath,
                commitPath,
                std::filesystem::copy_options::overwrite_existing,
                copyError);
            if (copyError)
            {
                a_error = std::format("Could not prepare the menu layout for saving: {}", copyError.message());
                return false;
            }
            if (!::MoveFileExW(
                    commitPath.c_str(),
                    a_session.sourcePath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const std::error_code moveError(static_cast<int>(::GetLastError()), std::system_category());
                std::error_code cleanupError;
                std::filesystem::remove(commitPath, cleanupError);
                a_error = std::format("Could not save the menu layout: {}", moveError.message());
                return false;
            }
            for (std::size_t index = 0; index < a_session.pageOrigins.size(); ++index)
                a_session.pageOrigins[index] = index;
            a_session.dirty = false;
            return true;
        }

        bool RestoreLayoutEditSession(LayoutEditSession& a_session, std::string& a_error)
        {
            a_error.clear();
            std::error_code copyError;
            std::filesystem::copy_file(
                a_session.sourcePath,
                a_session.workingPath,
                std::filesystem::copy_options::overwrite_existing,
                copyError);
            if (copyError)
            {
                a_error = std::format("Could not restore the saved menu layout: {}", copyError.message());
                return false;
            }
            std::string loadError;
            const auto pages = SliderCreator::Load(a_session.sourcePath, loadError);
            if (!loadError.empty())
            {
                a_error = std::format("Could not read the restored menu pages: {}", loadError);
                return false;
            }
            a_session.pageOrigins.clear();
            a_session.pageOrigins.reserve(pages.size());
            for (std::size_t index = 0; index < pages.size(); ++index)
                a_session.pageOrigins.emplace_back(index);
            a_session.dirty = false;
            return true;
        }

        bool LayoutFilesMatch(const LayoutEditSession& a_session)
        {
            std::error_code error;
            if (std::filesystem::file_size(a_session.sourcePath, error) !=
                    std::filesystem::file_size(a_session.workingPath, error) ||
                error)
                return false;
            std::ifstream source(a_session.sourcePath, std::ios::binary);
            std::ifstream working(a_session.workingPath, std::ios::binary);
            return source && working &&
                   std::equal(
                       std::istreambuf_iterator<char>(source),
                       std::istreambuf_iterator<char>(),
                       std::istreambuf_iterator<char>(working),
                       std::istreambuf_iterator<char>());
        }

        void RefreshLayoutDirtyState(LayoutEditSession& a_session)
        {
            a_session.dirty = !LayoutFilesMatch(a_session);
        }

        void DiscardLayoutEditSession(const std::string_view a_profile)
        {
            const auto session = layoutEditSessions.find(std::string(a_profile));
            if (session == layoutEditSessions.end()) return;
            std::error_code error;
            std::filesystem::remove(session->second.workingPath, error);
            std::filesystem::remove(LayoutCommitPath(session->second.sourcePath), error);
            layoutEditSessions.erase(session);
        }

        void DiscardAllLayoutEditSessions()
        {
            if (layoutEditSessions.empty())
            {
                return;
            }

            std::vector<std::string> profiles;
            profiles.reserve(layoutEditSessions.size());
            for (const auto& [profile, session] : layoutEditSessions)
            {
                (void) session;
                profiles.push_back(profile);
            }
            for (const auto& profile : profiles) DiscardLayoutEditSession(profile);
            loadedDefinitionFiles.clear();
            nextDefinitionCheck = {};
        }

        std::string Trim(std::string a_value)
        {
            const auto first = a_value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
            {
                return {};
            }
            const auto last = a_value.find_last_not_of(" \t\r\n");
            return a_value.substr(first, last - first + 1);
        }

        std::string Lowercase(std::string a_value)
        {
            std::ranges::transform(
                a_value,
                a_value.begin(),
                [](const unsigned char a_character)
                {
                    return static_cast<char>(std::tolower(a_character));
                });
            return a_value;
        }

        std::optional<SKSEMenuSettings::Color> ControlButtonColor(const MenuControl& a_control)
        {
            return a_control.color ?
                       a_control.color :
                       SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::ordinary);
        }

        std::string ControlLabel(
            const MenuControl& a_control,
            const std::string_view a_key,
            const std::string_view a_fallback)
        {
            return a_control.label.empty() ? SKSEMenuSettings::Label(a_key, a_fallback) : a_control.label;
        }

        std::string ControlDisplayName(const MenuControl& a_control, std::string a_fallback)
        {
            return a_control.displayName.value_or(std::move(a_fallback));
        }

        void SameActionLine()
        {
            ImGuiMCP::SameLine(0.0f, SKSEMenuSettings::GetActionButtonSpacing());
        }

        void ObserveMenuPage(const std::string_view a_page)
        {
            if (!activeMenuPage.empty() && activeMenuPage != a_page && SKSEMenuSettings::ClearStatusOnPageChange())
            {
                statusMessage.clear();
                settingsStatusMessage.clear();
            }
            activeMenuPage = a_page;
        }

        std::string StatusText(
            const std::string_view a_key,
            const std::initializer_list<SKSEMenuSettings::MessageArgument> a_arguments = {})
        {
            return SKSEMenuSettings::StatusMessage(a_key, a_arguments);
        }

        std::string DisplayText(
            const std::string_view a_key,
            const std::initializer_list<SKSEMenuSettings::MessageArgument> a_arguments = {})
        {
            return SKSEMenuSettings::DisplayMessage(a_key, a_arguments);
        }

        void DrawHeader(const std::string_view a_text, const float a_scale = 1.0f)
        {
            const auto localScale = std::isfinite(a_scale) && a_scale > 0.0f ? a_scale : 1.0f;
            const auto scale = SKSEMenuSettings::GetHeaderFontScale() * localScale;
            const std::string text(a_text);
            if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(scale);
            ImGuiMCP::SeparatorText(text.c_str());
            if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(1.0f);
        }

        std::string SliderCreatorErrorText(const std::string_view a_error)
        {
            static constexpr std::array errors{
                std::pair{ "Enter a slider name.", "sliderCreatorEnterSliderName" },
                std::pair{ "The slider ID must contain only letters, numbers, underscores, or hyphens.", "sliderCreatorInvalidSliderID" },
                std::pair{ "Add at least one valid setting to the slider.", "sliderCreatorNoValidSettings" },
                std::pair{ "The slider contains a setting path that TuningUtil does not support.", "sliderCreatorUnsupportedSettingPath" },
                std::pair{ "All Hues is a creator shortcut; direct sliders must store its seven individual hue bands.", "sliderCreatorDirectAllHues" },
                std::pair{ "A direct link requires only linkable, unfiltered settings.", "sliderCreatorInvalidDirectLink" },
                std::pair{ "Filtered sliders support only weather brightness, saturation, and hue-shift settings.", "sliderCreatorFilteredUnsupportedSetting" },
                std::pair{ "Lighting Template filters support only interior brightness and Fog Strength settings.", "sliderCreatorFilteredLightingUnsupportedSetting" },
                std::pair{ "Base Light filters support only Point Lights settings.", "sliderCreatorFilteredBaseLightUnsupportedSetting" },
                std::pair{ "Time filters, local links, and saturation scales do not apply to Base Light filters.", "sliderCreatorBaseLightWeatherFeatures" },
                std::pair{ "Every setting in a filtered slider must use the same filter domain.", "sliderCreatorMixedFilterDomains" },
                std::pair{ "Effect Lighting weather filters do not support local links.", "sliderCreatorEffectLightingLocalLink" },
                std::pair{ "Time filters, local links, and saturation scales apply only to filtered weather sliders.", "sliderCreatorLightingWeatherFeatures" },
                std::pair{ "Every setting in a filtered slider must use the same operation.", "sliderCreatorMixedFilteredOperations" },
                std::pair{ "The local link must name a target supported by this filtered operation.", "sliderCreatorInvalidLocalLink" },
                std::pair{ "Slider-specific saturation scales are supported only by filtered saturation sliders.", "sliderCreatorHueScalesRequireSaturation" },
                std::pair{ "Local links and slider-specific saturation scales require a filtered weather slider.", "sliderCreatorLocalFeaturesRequireFilter" },
                std::pair{ "Every slider-specific saturation scale must be a finite number.", "sliderCreatorInvalidHueScale" },
                std::pair{ "Select at least one time of day or disable the time filter.", "sliderCreatorNoTimeSelected" },
                std::pair{ "The slider minimum must be lower than its maximum.", "sliderCreatorInvalidRange" },
                std::pair{ "The slider step cannot be negative.", "sliderCreatorNegativeStep" },
                std::pair{ "The updated menu JSON could not be serialized.", "sliderCreatorSerializeFailure" },
                std::pair{ "The temporary menu file could not be written.", "sliderCreatorTemporaryWriteFailure" },
                std::pair{ "The menu file does not contain a pages array.", "sliderCreatorPagesMissing" },
                std::pair{ "Enter a valid profile name that can be used as a Windows folder name.", "sliderCreatorInvalidProfileName" },
                std::pair{ "Luma/Tuning/newSkseMenu.json is missing or does not contain a valid pages array.", "sliderCreatorTemplateMissing" },
                std::pair{ "A profile folder with that name already exists.", "sliderCreatorProfileExists" },
                std::pair{ "The profile folder could not be created.", "sliderCreatorProfileFolderUnknownFailure" },
                std::pair{ "The new profile's skseMenu.json could not be written.", "sliderCreatorMenuWriteFailure" },
                std::pair{ "The new profile's profileSettings.json could not be written.", "sliderCreatorProfileSettingsWriteFailure" },
                std::pair{ "Enter a page name.", "sliderCreatorEnterPageName" },
                std::pair{ "A page with that name already exists.", "sliderCreatorPageExists" },
                std::pair{ "The new page JSON could not be created.", "sliderCreatorPageJsonFailure" },
                std::pair{ "The menu file could not be read.", "sliderCreatorMenuReadFailure" },
                std::pair{ "The menu file could not be copied for editing.", "sliderCreatorMenuCopyFailure" },
                std::pair{ "The selected page is unavailable.", "sliderCreatorPageUnavailable" },
                std::pair{ "The page visibility could not be changed.", "sliderCreatorPageAdvancedFailure" },
                std::pair{ "Another slider already uses this ID.", "sliderCreatorDuplicateID" },
                std::pair{ "The slider JSON could not be created.", "sliderCreatorSliderJsonFailure" },
                std::pair{ "The slider being edited no longer exists.", "sliderCreatorEditedSliderMissing" },
                std::pair{ "The slider could not be added to the selected page.", "sliderCreatorAppendFailure" },
                std::pair{ "Select a module to add.", "layoutSelectModule" },
                std::pair{ "The menu layout could not be read.", "layoutReadFailure" },
                std::pair{ "The module could not be added to this page.", "layoutModuleAddFailure" },
                std::pair{ "The module cannot move farther in that direction.", "layoutModuleMoveBoundary" },
                std::pair{ "The module order could not be changed.", "layoutModuleMoveFailure" },
                std::pair{ "The module could not be removed.", "layoutModuleRemoveFailure" },
                std::pair{ "The selected module is unavailable.", "layoutModuleUnavailable" },
                std::pair{ "This module does not have a display name.", "layoutModuleNameUnavailable" },
                std::pair{ "The module could not be renamed.", "layoutModuleRenameFailure" },
                std::pair{ "The page cannot move farther in that direction.", "layoutPageMoveBoundary" },
                std::pair{ "The page order could not be changed.", "layoutPageMoveFailure" },
                std::pair{ "The page could not be renamed.", "layoutPageRenameFailure" },
                std::pair{ "The page could not be removed.", "layoutPageRemoveFailure" },
            };
            const auto found = std::ranges::find_if(errors, [&](const auto& a_entry)
                { return a_entry.first == a_error; });
            if (found != errors.end()) return DisplayText(found->second);

            static constexpr std::array prefixedErrors{
                std::pair{ "The menu file could not be replaced: ", "sliderCreatorReplaceFailure" },
                std::pair{ "The profile folder could not be created: ", "sliderCreatorProfileFolderFailure" },
            };
            for (const auto& [prefix, key] : prefixedErrors)
                if (a_error.starts_with(prefix))
                    return DisplayText(key, { { "reason", std::string(a_error.substr(std::string_view(prefix).size())) } });
            return std::string(a_error);
        }

        void DrawDisplayText(
            const std::string_view a_key,
            const bool a_disabled = false,
            const std::initializer_list<SKSEMenuSettings::MessageArgument> a_arguments = {})
        {
            const auto message = DisplayText(a_key, a_arguments);
            if (message.empty())
            {
                return;
            }
            if (a_disabled) ImGuiMCP::TextDisabled("%s", message.c_str());
            else ImGuiMCP::TextWrapped("%s", message.c_str());
        }

        void DrawStatusText(const TimedMessage& a_message)
        {
            const auto color = SKSEMenuSettings::GetStatusColor();
            if (color)
            {
                ImGuiMCP::PushStyleColor(
                    ImGuiMCP::ImGuiCol_Text,
                    ImGuiMCP::ImVec4((*color)[0], (*color)[1], (*color)[2], (*color)[3]));
            }
            const auto scale = SKSEMenuSettings::GetStatusFontScale();
            if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(scale);
            ImGuiMCP::TextWrapped("%s", a_message.c_str());
            if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(1.0f);
            if (color) ImGuiMCP::PopStyleColor();
        }

        void DrawStatusMessage(TimedMessage& a_message, const std::string_view a_reservedAreaId = {})
        {
            if (SKSEMenuSettings::GetStatusLocation() == SKSEMenuSettings::StatusLocation::hidden)
            {
                return;
            }
            const auto duration = SKSEMenuSettings::GetStatusDuration();
            if (!a_message.empty() && duration.count() > 0.0 &&
                std::chrono::steady_clock::now() - a_message.changedAt >= duration)
            {
                a_message.clear();
            }

            if (!a_reservedAreaId.empty())
            {
                const auto id = std::string(a_reservedAreaId);
                constexpr auto windowFlags = ImGuiMCP::ImGuiWindowFlags_NoScrollbar |
                                             ImGuiMCP::ImGuiWindowFlags_NoScrollWithMouse;
                if (ImGuiMCP::BeginChild(
                        id.c_str(),
                        ImGuiMCP::ImVec2(0.0f, SKSEMenuSettings::GetStatusHeight()),
                        ImGuiMCP::ImGuiChildFlags_None,
                        windowFlags) &&
                    !a_message.empty())
                {
                    DrawStatusText(a_message);
                }
                ImGuiMCP::EndChild();
                return;
            }

            if (a_message.empty()) return;
            ImGuiMCP::Spacing();
            DrawStatusText(a_message);
        }

        bool AffectsLightPlacer(const std::string_view a_setting)
        {
            return a_setting == "pointLights" || a_setting.starts_with("pointLights.") ||
                   a_setting.starts_with("filteredBaseLightAdjustments.") ||
                   a_setting == "intHueRanges" || a_setting.starts_with("intHueRanges.");
        }

        void ApplySliderChange(const bool a_affectsLightPlacer)
        {
            TuningUtil::ApplySettings(!a_affectsLightPlacer);
            lightPlacerCommitPending |= a_affectsLightPlacer;
        }

        void CommitLightPlacerAfterSliderRelease()
        {
            if (lightPlacerCommitPending && ImGuiMCP::IsMouseReleased(ImGuiMCP::ImGuiMouseButton_Left))
            {
                lightPlacerCommitPending = false;
                TuningUtil::ApplySettings(true);
            }
        }

        bool UsesWeatherMenuRuntime(const MenuControl& a_control)
        {
            return a_control.type == "weatherSelector" ||
                   a_control.type == "weatherControlCompact" ||
                   a_control.type == "weatherSliderCreator" ||
                   a_control.type == "ambientWithinGauge" ||
                   a_control.type == "ambientBetweenGauge" ||
                   a_control.type == "sunlightWithinGauge" ||
                   a_control.type == "sunlightBetweenGauge" ||
                   a_control.type == "dynamicAmbientWeatherList";
        }

        bool HasWeatherMenuControls(const MenuDefinition& a_menu)
        {
            return std::ranges::any_of(a_menu.pages, [](const MenuPage& a_page)
                { return std::ranges::any_of(a_page.modules, UsesWeatherMenuRuntime); });
        }

        bool HasModuleKind(const MenuDefinition& a_menu, const std::string_view a_kind)
        {
            const auto matches = [&](const MenuControl& a_module)
            { return a_module.type == a_kind; };
            return std::ranges::any_of(a_menu.pages, [&](const MenuPage& a_page)
                { return std::ranges::any_of(a_page.modules, matches); });
        }

        bool ReadFrameworkBoolean(const std::string_view a_key, const bool a_default)
        {
            std::ifstream file(kMenuFrameworkIni);
            if (!file)
            {
                return a_default;
            }

            auto inGeneralSection = false;
            std::string line;
            while (std::getline(file, line))
            {
                line = Trim(std::move(line));
                if (line.empty() || line.starts_with(';') || line.starts_with('#'))
                {
                    continue;
                }
                if (line.starts_with('[') && line.ends_with(']'))
                {
                    inGeneralSection = Lowercase(Trim(line.substr(1, line.size() - 2))) == "general";
                    continue;
                }
                if (!inGeneralSection)
                {
                    continue;
                }

                const auto separator = line.find('=');
                if (separator == std::string::npos ||
                    Lowercase(Trim(line.substr(0, separator))) != Lowercase(std::string{ a_key }))
                {
                    continue;
                }

                const auto value = Lowercase(Trim(line.substr(separator + 1)));
                if (value == "true" || value == "1" || value == "yes" || value == "on")
                {
                    return true;
                }
                if (value == "false" || value == "0" || value == "no" || value == "off")
                {
                    return false;
                }
                return a_default;
            }
            return a_default;
        }

        void DisableFrameworkEffectsForLuma()
        {
            menuEffectOverride.lumaRenderedThisFrame = true;

            if (menuEffectOverride.freezeTimeEnabled)
            {
                if (auto* main = RE::Main::GetSingleton())
                {
                    main->GetRuntimeData().freezeTime = false;
                }
            }

            if (menuEffectOverride.active)
            {
                return;
            }

            menuEffectOverride.active = true;
            if (menuEffectOverride.backgroundBlurEnabled)
            {
                if (auto* blur = RE::UIBlurManager::GetSingleton())
                {
                    if (blur->blurCount > 0)
                    {
                        blur->DecrementBlurCount();
                        menuEffectOverride.blurContributionRemoved = true;
                    }
                }
            }
            logger::debug("[Tuning Menu] effects | freeze=false | blur=false | temporary=true");
        }

        void RestoreFrameworkEffects()
        {
            if (!menuEffectOverride.active)
            {
                return;
            }

            if (menuEffectOverride.freezeTimeEnabled)
            {
                if (auto* main = RE::Main::GetSingleton())
                {
                    main->GetRuntimeData().freezeTime = true;
                }
            }

            if (menuEffectOverride.blurContributionRemoved)
            {
                if (auto* blur = RE::UIBlurManager::GetSingleton())
                {
                    blur->IncrementBlurCount();
                }
            }

            menuEffectOverride.active = false;
            menuEffectOverride.blurContributionRemoved = false;
            logger::debug("[Tuning Menu] effects | status=restored");
        }

        void ReleaseFrameworkEffectsAfterClose()
        {
            if (!menuEffectOverride.active)
            {
                return;
            }

            // Menu Framework owns the final release for the now-closed window later in this render cycle.
            menuEffectOverride.active = false;
            menuEffectOverride.blurContributionRemoved = false;
        }

        void InitializeOpenedMenuState()
        {
            if (WeatherLock::IsEnabled())
            {
                WeatherLock::SetEnabled(false);
            }
            WeatherLock::SetSelectedWeather(nullptr);
            activeWeatherLockProfile.clear();
        }

        void FinalizeClosedMenuState(const bool a_menuReopened)
        {
            DiscardAllLayoutEditSessions();
            if (lightPlacerCommitPending)
            {
                lightPlacerCommitPending = false;
                TuningUtil::ApplySettings(true);
            }
            WeatherLock::SetEnabled(false);
            WeatherLock::SetSelectedWeather(nullptr);
            activeWeatherLockProfile.clear();
            statusMessage.clear();
            settingsStatusMessage.clear();
            if (a_menuReopened)
            {
                RestoreFrameworkEffects();
            }
            else
            {
                ReleaseFrameworkEffectsAfterClose();
            }
            menuEffectOverride.lumaRenderedThisFrame = false;
        }

        void __stdcall HandleMenuFrameworkEvent(const SKSEMenuFramework::Model::EventType a_event)
        {
            switch (a_event)
            {
            case SKSEMenuFramework::Model::kOpenMenu:
                menuFrameworkOpen.store(true, std::memory_order_release);
                menuEffectOverride.openInitializationPending.store(true, std::memory_order_release);
                break;
            case SKSEMenuFramework::Model::kBeforeRender:
                if (menuEffectOverride.closeCleanupPending.exchange(false, std::memory_order_acq_rel))
                {
                    FinalizeClosedMenuState(menuFrameworkOpen.load(std::memory_order_acquire));
                }
                if (menuEffectOverride.openInitializationPending.exchange(false, std::memory_order_acq_rel))
                {
                    InitializeOpenedMenuState();
                }
                menuEffectOverride.lumaRenderedThisFrame = false;
                if (menuFrameworkOpen.load(std::memory_order_acquire))
                {
                    WeatherLock::Maintain();
                }
                break;
            case SKSEMenuFramework::Model::kAfterRender:
                if (menuFrameworkOpen.load(std::memory_order_acquire) &&
                    !menuEffectOverride.lumaRenderedThisFrame)
                {
                    RestoreFrameworkEffects();
                }
                break;
            case SKSEMenuFramework::Model::kCloseMenu:
                menuFrameworkOpen.store(false, std::memory_order_release);
                menuEffectOverride.openInitializationPending.store(false, std::memory_order_release);
                menuEffectOverride.closeCleanupPending.store(true, std::memory_order_release);
                break;
            default:
                break;
            }
        }

        bool IsSafeSliderFormat(const std::string_view a_format)
        {
            auto conversionFound = false;
            for (std::size_t index = 0; index < a_format.size(); ++index)
            {
                if (a_format[index] != '%')
                {
                    continue;
                }
                if (index + 1 < a_format.size() && a_format[index + 1] == '%')
                {
                    ++index;
                    continue;
                }
                if (conversionFound)
                {
                    return false;
                }
                conversionFound = true;

                constexpr std::string_view flagsAndPrecision = "-+ #0.123456789";
                while (++index < a_format.size() && flagsAndPrecision.contains(a_format[index]))
                {
                }
                if (index >= a_format.size() || !std::string_view{ "aAeEfFgG" }.contains(a_format[index]))
                {
                    return false;
                }
            }
            return conversionFound;
        }

        struct SliderRange
        {
            float minimum;
            float maximum;
        };

        SliderRange FallbackSliderRange(const std::string_view a_setting)
        {
            const auto setting = Lowercase(std::string(a_setting));
            if (setting.contains("huerange")) return { 0.0f, 255.0f };
            if (setting.contains("hueshift")) return { -180.0f, 180.0f };
            if (setting.contains("compressionanchor")) return { 0.0f, 255.0f };
            if (setting.contains("compression")) return { -200.0f, 100.0f };
            if (setting.contains("saturation")) return { 0.0f, 6.0f };
            if (setting.contains("intbrightness")) return { 0.0f, 10.0f };
            if (setting.contains("brightnessmultiplier")) return { 0.1f, 4.0f };
            if (setting.contains("multiplier") || setting.contains("huescale")) return { 0.0f, 4.0f };
            return { 0.0f, 2.0f };
        }

        std::string SliderFormatFromStep(const float a_step, const std::string_view a_fallback)
        {
            if (!std::isfinite(a_step) || a_step <= 0.0f) return std::string(a_fallback);

            auto scaledStep = static_cast<double>(a_step);
            for (auto precision = 0; precision <= 6; ++precision)
            {
                const auto roundedStep = std::round(scaledStep);
                const auto tolerance = std::max(1e-6, std::abs(scaledStep) * 1e-5);
                if (std::abs(scaledStep - roundedStep) <= tolerance)
                    return std::format("%.{}f", precision);
                scaledStep *= 10.0;
            }
            return "%.6f";
        }

        SKSEMenuSettings::SliderDefaults ResolveControlSliderDefaults(
            const MenuControl& a_control,
            const std::string_view a_setting,
            const float a_fallbackWidth = 0.0f,
            const float a_fallbackStep = 0.0f,
            const std::string_view a_fallbackFormat = "%.2f")
        {
            const auto range = FallbackSliderRange(a_setting);
            auto result = SKSEMenuSettings::ResolveSliderDefaults(
                a_setting,
                a_fallbackWidth,
                a_fallbackStep,
                a_fallbackFormat,
                range.minimum,
                range.maximum);
            if (std::isfinite(a_control.min)) result.minimum = a_control.min;
            if (std::isfinite(a_control.max)) result.maximum = a_control.max;
            if (std::isfinite(a_control.width)) result.width = a_control.width;
            if (std::isfinite(a_control.step))
            {
                result.step = std::max(0.0f, a_control.step);
                if (a_control.format.empty()) result.format = SliderFormatFromStep(result.step, result.format);
            }
            if (!a_control.format.empty()) result.format = a_control.format;
            const auto settingName = Lowercase(std::string(a_setting));
            if (settingName.contains("compression") && !settingName.contains("compressionanchor"))
            {
                result.maximum = std::min(result.maximum, 100.0f);
            }
            return result;
        }

        float SnapSliderValue(
            const float a_value,
            const float a_minimum,
            const float a_maximum,
            const float a_step)
        {
            if (a_step <= 0.0f)
            {
                return std::clamp(a_value, a_minimum, a_maximum);
            }
            const auto steps = std::round((a_value - a_minimum) / a_step);
            return std::clamp(a_minimum + (steps * a_step), a_minimum, a_maximum);
        }

        std::optional<float> SliderNeutralValue(const std::string_view a_setting)
        {
            const auto neutralValue = SliderSettingCatalog::NeutralValue(a_setting);
            if (!neutralValue || !std::isfinite(*neutralValue))
            {
                return std::nullopt;
            }
            return static_cast<float>(*neutralValue);
        }

        std::optional<float> ControlNeutralValue(const MenuControl& a_control)
        {
            if (a_control.invert)
            {
                return std::nullopt;
            }
            if (a_control.settings.empty())
            {
                return SliderNeutralValue(a_control.setting);
            }

            std::optional<float> result;
            for (const auto& target : a_control.settings)
            {
                const auto neutral = SliderNeutralValue(SliderTargetPath(target));
                if (!neutral || (result && std::abs(*result - *neutral) > 0.0001f))
                {
                    return std::nullopt;
                }
                result = neutral;
            }
            return result;
        }

        bool HasCenteredNeutral(
            const std::optional<float> a_neutral,
            const float a_minimum,
            const float a_maximum)
        {
            return a_neutral && std::isfinite(*a_neutral) &&
                   *a_neutral > a_minimum && *a_neutral < a_maximum;
        }

        float SliderPositionFromValue(
            const float a_value,
            const float a_minimum,
            const float a_maximum,
            const float a_neutral)
        {
            const auto value = std::clamp(a_value, a_minimum, a_maximum);
            if (value <= a_neutral)
            {
                return 0.5f * (value - a_minimum) / (a_neutral - a_minimum);
            }
            return 0.5f + (0.5f * (value - a_neutral) / (a_maximum - a_neutral));
        }

        float SliderValueFromPosition(
            const float a_position,
            const float a_minimum,
            const float a_maximum,
            const float a_neutral)
        {
            const auto position = std::clamp(a_position, 0.0f, 1.0f);
            if (position <= 0.5f)
            {
                return a_minimum + ((a_neutral - a_minimum) * position * 2.0f);
            }
            return a_neutral + ((a_maximum - a_neutral) * (position - 0.5f) * 2.0f);
        }

        enum class SliderInputRange
        {
            standard,
            hue,
        };

        bool DrawSliderWithInput(
            const std::string& a_id,
            float& a_value,
            const float a_minimum,
            const float a_maximum,
            const float a_step,
            const char* a_format,
            const float a_width = 0.0f,
            const SliderInputRange a_inputRange = SliderInputRange::standard,
            const std::optional<float> a_neutralValue = std::nullopt)
        {
            constexpr auto inputWidth = 80.0f;
            auto changed = false;
            auto sliderValue = std::clamp(a_value, a_minimum, a_maximum);
            const auto labelSeparator = a_id.find("##");
            const auto visibleLabel = a_id.substr(0, labelSeparator);
            const auto sliderID = "##Slider" + a_id;
            if (!visibleLabel.empty()) ImGuiMCP::TextUnformatted(visibleLabel.c_str());
            if (a_width != 0.0f) ImGuiMCP::SetNextItemWidth(a_width);
            const auto centered = HasCenteredNeutral(a_neutralValue, a_minimum, a_maximum);
            auto sliderPosition = centered ?
                                      SliderPositionFromValue(
                                          sliderValue,
                                          a_minimum,
                                          a_maximum,
                                          *a_neutralValue) :
                                      sliderValue;
            if (ImGuiMCP::SliderFloat(
                    sliderID.c_str(),
                    &sliderPosition,
                    centered ? 0.0f : a_minimum,
                    centered ? 1.0f : a_maximum,
                    ""))
            {
                const auto rawValue = centered ?
                                          SliderValueFromPosition(
                                              sliderPosition,
                                              a_minimum,
                                              a_maximum,
                                              *a_neutralValue) :
                                          sliderPosition;
                a_value = centered && a_step > 0.0f &&
                                  std::abs(rawValue - *a_neutralValue) <= a_step * 0.5f ?
                              *a_neutralValue :
                              SnapSliderValue(rawValue, a_minimum, a_maximum, a_step);
                changed = true;
            }

            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(inputWidth);
            auto inputValue = a_value;
            const auto inputMinimum =
                a_inputRange == SliderInputRange::hue ? 0.0f : a_minimum < 0.0f ? std::min(-99.0f, a_minimum) :
                                                                                 0.0f;
            const auto inputMaximum =
                a_inputRange == SliderInputRange::hue ? 255.0f : std::max(99.0f, a_maximum);
            if (ImGuiMCP::InputFloat(
                    ("##ValueInput" + a_id).c_str(),
                    &inputValue,
                    0.0f,
                    0.0f,
                    a_format))
            {
                a_value = std::clamp(inputValue, inputMinimum, inputMaximum);
                changed = true;
            }
            return changed;
        }

        bool DrawConfiguredSlider(
            const std::string_view a_setting,
            const std::string& a_id,
            float& a_value,
            const float a_minimum,
            const float a_maximum,
            const float a_fallbackStep,
            const std::string_view a_fallbackFormat,
            const float a_fallbackWidth = 0.0f)
        {
            const auto style = SKSEMenuSettings::ResolveSliderDefaults(
                a_setting,
                a_fallbackWidth,
                a_fallbackStep,
                a_fallbackFormat);
            const auto fallbackFormat = std::string(a_fallbackFormat);
            const auto* format = IsSafeSliderFormat(style.format) ? style.format.c_str() : fallbackFormat.c_str();
            return DrawSliderWithInput(
                a_id,
                a_value,
                a_minimum,
                a_maximum,
                style.step,
                format,
                style.width,
                SliderInputRange::standard,
                SliderNeutralValue(a_setting));
        }

        void DrawDynamicAmbientRangeDisplay(
            const std::string_view a_setting,
            const float a_darkLimit,
            const float a_brightLimit)
        {
            const auto style = SKSEMenuSettings::ResolveSliderDefaults(
                a_setting,
                420.0f,
                1.0f,
                "%.0f",
                0.0f,
                255.0f);
            const float width = style.width > 0.0f ? style.width : 420.0f;
            const float height = ImGuiMCP::GetFrameHeight();
            const auto position = ImGuiMCP::GetCursorScreenPos();
            const auto* imguiStyle = ImGuiMCP::GetStyle();
            const float handleThickness = std::clamp(imguiStyle ? imguiStyle->GrabMinSize : 10.0f, 6.0f, height);
            ImGuiMCP::Dummy(ImGuiMCP::ImVec2(width, height));

            auto* drawList = ImGuiMCP::GetWindowDrawList();
            const auto frameColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImGuiCol_FrameBg);
            const auto white = ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            const ImGuiMCP::ImVec2 minimum(position.x, position.y);
            const ImGuiMCP::ImVec2 maximum(position.x + width, position.y + height);
            const float darkX = position.x + (width * a_darkLimit / 255.0f);
            const float brightX = position.x + (width * a_brightLimit / 255.0f);
            const auto* normalRangeColor = ImGuiMCP::GetStyleColorVec4(ImGuiMCP::ImGuiCol_SliderGrab);
            const auto rangeColor = ImGuiMCP::GetColorU32(
                normalRangeColor ?
                    *normalRangeColor :
                    ImGuiMCP::ImVec4(0.24f, 0.52f, 0.88f, 1.0f));
            ImGuiMCP::ImDrawListManager::AddRectFilled(drawList, minimum, maximum, frameColor, 3.0f, 0);
            ImGuiMCP::ImDrawListManager::AddRectFilled(
                drawList,
                ImGuiMCP::ImVec2(darkX, position.y),
                ImGuiMCP::ImVec2(brightX, position.y + height),
                rangeColor,
                0.0f,
                0);
            ImGuiMCP::ImDrawListManager::AddLine(
                drawList,
                ImGuiMCP::ImVec2(darkX, position.y - 1.0f),
                ImGuiMCP::ImVec2(darkX, position.y + height + 1.0f),
                white,
                handleThickness);
            ImGuiMCP::ImDrawListManager::AddLine(
                drawList,
                ImGuiMCP::ImVec2(brightX, position.y - 1.0f),
                ImGuiMCP::ImVec2(brightX, position.y + height + 1.0f),
                white,
                handleThickness);
        }

        void DrawDynamicBrightnessModule(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const WeatherPatcher::DynamicAmbientMode a_mode,
            const WeatherPatcher::DynamicBrightnessField a_field)
        {
            auto profile = a_menu.profile;
            const bool ambient = a_field == WeatherPatcher::DynamicBrightnessField::ambient;
            const auto stateKey =
                profile +
                (ambient ? "Ambient" : "Sunlight") +
                (a_mode == WeatherPatcher::DynamicAmbientMode::within ? "Within" : "Between");
            auto& state = dynamicAmbientModuleStates[stateKey];
            const auto refreshState = [&]()
            {
                state.status = WeatherPatcher::GetDynamicBrightnessStatus(profile, a_mode, a_field);
                state.range = state.status ?
                                  state.status->source :
                                  WeatherPatcher::GetDynamicAmbientRange(profile, a_mode, a_field);
                state.settingsRevision = TuningUtil::GetSettingsRevision();
            };
            const auto revision = TuningUtil::GetSettingsRevision();
            if (state.settingsRevision != revision)
            {
                refreshState();
            }

            const auto labelKey =
                ambient ?
                    (a_mode == WeatherPatcher::DynamicAmbientMode::within ?
                            "ambientWithinGauge" :
                            "ambientBetweenGauge") :
                    (a_mode == WeatherPatcher::DynamicAmbientMode::within ?
                            "sunlightWithinGauge" :
                            "sunlightBetweenGauge");
            const auto fallbackLabel =
                ambient ?
                    (a_mode == WeatherPatcher::DynamicAmbientMode::within ?
                            "Ambient Brightness Within Weather Gauge" :
                            "Ambient Brightness Between Weather Gauge") :
                    (a_mode == WeatherPatcher::DynamicAmbientMode::within ?
                            "Sunlight Brightness Within Weather Gauge" :
                            "Sunlight Brightness Between Weather Gauge");
            const auto displayName = ControlDisplayName(
                a_control,
                ControlLabel(a_control, labelKey, fallbackLabel));
            if (!displayName.empty()) ImGuiMCP::TextUnformatted(displayName.c_str());
            const auto& result = state.status ? state.status->result : state.range;
            if (!result.available)
            {
                ImGuiMCP::BeginDisabled();
            }

            float darkLimit = static_cast<float>(result.darkLimit);
            float brightLimit = static_cast<float>(result.brightLimit);
            darkLimit = std::clamp(darkLimit, 0.0f, 255.0f);
            brightLimit = std::clamp(brightLimit, darkLimit, 255.0f);
            DrawDynamicAmbientRangeDisplay(labelKey, darkLimit, brightLimit);
            if (!result.available)
            {
                ImGuiMCP::EndDisabled();
            }
        }

        void DrawDynamicBrightnessWeatherList(
            const MenuDefinition& a_menu,
            const WeatherPatcher::DynamicBrightnessField a_field)
        {
            auto profile = a_menu.profile;
            const auto revision = TuningUtil::GetSettingsRevision();
            const auto loadRange = [&](const WeatherPatcher::DynamicAmbientMode a_mode)
            {
                const auto stateKey =
                    profile +
                    (a_field == WeatherPatcher::DynamicBrightnessField::ambient ? "Ambient" : "Sunlight") +
                    (a_mode == WeatherPatcher::DynamicAmbientMode::within ? "Within" : "Between");
                auto& state = dynamicAmbientModuleStates[stateKey];
                if (state.settingsRevision != revision)
                {
                    state.range = WeatherPatcher::GetDynamicAmbientRange(profile, a_mode, a_field);
                    state.settingsRevision = revision;
                }
                return state.range;
            };

            const auto weatherName = [](const RE::TESWeather* a_weather)
            {
                return a_weather ?
                           WeatherPatcher::WeatherName(a_weather) :
                           DisplayText("weatherUnavailableLabel");
            };
            const auto within = loadRange(WeatherPatcher::DynamicAmbientMode::within);
            const auto between = loadRange(WeatherPatcher::DynamicAmbientMode::between);
            ImGuiMCP::Text(
                "%s: %s",
                SKSEMenuSettings::Label("brightestWeather", "Brightest Weather").c_str(),
                weatherName(between.brightWeather).c_str());
            ImGuiMCP::Text(
                "%s: %s",
                SKSEMenuSettings::Label("withinDarkestWeather", "Within Darkest Weather").c_str(),
                weatherName(within.darkWeather).c_str());
            ImGuiMCP::Text(
                "%s: %s",
                SKSEMenuSettings::Label("betweenDarkestWeather", "Between Darkest Weather").c_str(),
                weatherName(between.darkWeather).c_str());
        }

        std::optional<MenuDefinition> ReadMenuDefinition(
            const std::filesystem::path& a_path,
            const std::string_view a_profile)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file)
            {
                logger::warn("[Tuning Menu] definition open failed | path={}", a_path.string());
                return std::nullopt;
            }

            std::string text(std::istreambuf_iterator<char>(file), {});
            constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";
            if (text.starts_with(utf8Bom))
            {
                text.erase(0, utf8Bom.size());
            }

            auto parsed = rfl::json::read<MenuDefinition, rfl::DefaultIfMissing>(text);
            if (!parsed)
            {
                logger::warn("[Tuning Menu] definition load failed | path={} | {}", a_path.string(), parsed.error().what());
                return std::nullopt;
            }

            auto definition = parsed.value();
            if (definition.schemaVersion != 1)
            {
                logger::warn(
                    "[Tuning Menu] definition ignored | path={} | schemaVersion != 1",
                    a_path.string());
                return std::nullopt;
            }
            if (!definition.enabled)
            {
                return std::nullopt;
            }
            definition.profile = a_profile;
            if (definition.title.empty())
            {
                definition.title = definition.profile;
            }
            definition.profilePage.title = "Profile";
            definition.profilePage.order = std::min(definition.profilePage.order, definition.pages.size());
            for (const auto requiredKind : SliderCreator::kRequiredProfileModuleKinds)
            {
                if (!std::ranges::any_of(definition.profilePage.modules, [&](const MenuControl& a_module)
                    { return Config::IEquals(a_module.type, requiredKind); }))
                {
                    definition.profilePage.modules.push_back({ .type = std::string(requiredKind) });
                }
            }
            const auto normalizeControlColors = [](std::vector<MenuControl>& a_controls)
            {
                for (auto& control : a_controls)
                {
                    if (control.color) SKSEMenuSettings::NormalizeJsonColor(*control.color);
                }
            };
            normalizeControlColors(definition.profilePage.modules);
            for (auto& page : definition.pages) normalizeControlColors(page.modules);
            return definition;
        }

        std::vector<DefinitionFile> DiscoverDefinitionFiles()
        {
            std::vector<DefinitionFile> result;
            std::error_code ec;
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator iterator(kProfileRoot, options, ec), end;
                iterator != end && !ec;
                iterator.increment(ec))
            {
                if (!iterator->is_regular_file(ec) || iterator->path().filename().string() != kMenuDefinitionFileName)
                {
                    continue;
                }
                const auto writeTime = iterator->last_write_time(ec);
                if (!ec)
                {
                    result.push_back({ iterator->path(), writeTime });
                }
            }
            std::ranges::sort(result, {}, &DefinitionFile::path);
            return result;
        }

        void ReloadProfileMenus(const bool a_initialLoad)
        {
            const auto files = DiscoverDefinitionFiles();
            if (!a_initialLoad && files == loadedDefinitionFiles)
            {
                return;
            }

            const auto filteredRulesChanged = TuningUtil::ReloadFilteredRules();

            std::vector<LoadedMenu> loaded;
            const auto& tuningProfiles = TuningUtil::GetProfiles();
            for (const auto& file : files)
            {
                const auto profile = std::ranges::find_if(tuningProfiles, [&](const TuningUtil::Profile& a_candidate)
                    { return a_candidate.directory == file.path.parent_path(); });
                if (profile == tuningProfiles.end())
                {
                    if (!TuningUtil::IsProfilePluginFiltered(file.path.parent_path()))
                    {
                        logger::warn(
                            "[Tuning Menu] definition ignored | path={} | profileSettings missing",
                            file.path.string());
                    }
                    continue;
                }
                if (auto definition = ReadMenuDefinition(
                        ActiveLayoutPath(profile->name, file.path),
                        profile->name))
                {
                    loaded.push_back({ file.path, std::move(*definition) });
                }
            }

            std::ranges::sort(
                loaded,
                [](const LoadedMenu& a_left, const LoadedMenu& a_right)
                {
                    const auto leftPriority = TuningUtil::GetProfilePriority(a_left.definition.profile);
                    const auto rightPriority = TuningUtil::GetProfilePriority(a_right.definition.profile);
                    if (leftPriority != rightPriority)
                    {
                        return leftPriority < rightPriority;
                    }
                    const auto left = Lowercase(a_left.definition.title);
                    const auto right = Lowercase(a_right.definition.title);
                    return left != right ? left < right : a_left.definition.title < a_right.definition.title;
                });
            profileMenus = std::move(loaded);
            loadedDefinitionFiles = files;
            weatherMenuEntries.clear();
            sliderCreatorWeatherEntries.clear();
            recordFilterEditorStates.clear();
            presetVisualStates.clear();
            activeProfilePages.clear();

            logger::info(
                "[Tuning Menu] definitions={} | root={}",
                profileMenus.size(),
                kProfileRoot.string());
            if (filteredRulesChanged)
            {
                TuningUtil::ApplySettings();
            }
            if (!a_initialLoad)
            {
                statusMessage = pendingMenuReloadStatus ?
                                    std::move(*pendingMenuReloadStatus) :
                                    StatusText("profileMenusReloaded", { { "count", std::to_string(profileMenus.size()) } });
                pendingMenuReloadStatus.reset();
            }
        }

        void ReloadProfileMenusIfChanged()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextDefinitionCheck)
            {
                return;
            }
            nextDefinitionCheck = now + 1s;
            ReloadProfileMenus(false);
        }

        bool DrawSlider(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            const auto settingPath = !a_control.settings.empty() ?
                                         SliderTargetPath(a_control.settings.front()) :
                                         std::string_view(a_control.setting);
            auto setting = profile.empty() ?
                               std::nullopt :
                               FindSliderSetting(TuningUtil::GetSettings(profile), settingPath);
            if (!setting)
            {
                return false;
            }

            auto value = static_cast<float>(a_control.invert ? -setting->resolved : setting->resolved);
            auto sliderDefaults = ResolveControlSliderDefaults(a_control, settingPath);
            const auto minimum = std::min(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto maximum = std::max(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto* format = IsSafeSliderFormat(sliderDefaults.format) ? sliderDefaults.format.c_str() : "%.2f";
            if (DrawSliderWithInput(
                    a_id,
                    value,
                    minimum,
                    maximum,
                    sliderDefaults.step,
                    format,
                    sliderDefaults.width,
                    SliderInputRange::standard,
                    ControlNeutralValue(a_control)))
            {
                setting->Set(a_control.invert ? -value : value);
                ApplySliderChange(AffectsLightPlacer(settingPath));
            }
            return true;
        }

        bool DrawFilteredWeatherSlider(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            const auto* rule = TuningUtil::FindFilteredWeatherRule(profile, a_control.id);
            if (!rule)
            {
                return false;
            }

            auto& values = TuningUtil::GetSettings(profile).filteredWeatherAdjustments;
            auto [entry, inserted] = values.try_emplace(rule->id, rule->defaultValue);
            (void) inserted;
            auto value = static_cast<float>(a_control.invert ? -entry->second : entry->second);
            const auto setting = "filteredWeatherAdjustments." + rule->id;
            const auto styleSetting = rule->settings.empty() ? std::string_view(setting) : [&]() -> std::string_view
            {
                switch (rule->settings.front().operation)
                {
                case TuningUtil::FilteredWeatherOperation::brightness:
                    return "brightnessMultiplier";
                case TuningUtil::FilteredWeatherOperation::saturation:
                    return "saturationMultiplier";
                case TuningUtil::FilteredWeatherOperation::hueShift:
                    return "hueShift";
                }
                return setting;
            }();
            auto sliderDefaults = ResolveControlSliderDefaults(a_control, styleSetting);
            const auto minimum = std::min(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto maximum = std::max(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto* format = IsSafeSliderFormat(sliderDefaults.format) ? sliderDefaults.format.c_str() : "%.2f";
            if (DrawSliderWithInput(
                    a_id,
                    value,
                    minimum,
                    maximum,
                    sliderDefaults.step,
                    format,
                    sliderDefaults.width,
                    SliderInputRange::standard,
                    a_control.invert ? std::nullopt : SliderNeutralValue(styleSetting)))
            {
                entry->second = a_control.invert ? -value : value;
                TuningUtil::ApplySettings();
            }
            return true;
        }

        bool DrawFilteredLightingTemplateSlider(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            const auto* rule = TuningUtil::FindFilteredLightingTemplateRule(profile, a_control.id);
            if (!rule) return false;

            auto& values = TuningUtil::GetSettings(profile).filteredLightingTemplateAdjustments;
            auto [entry, inserted] = values.try_emplace(rule->id, rule->defaultValue);
            (void)inserted;
            auto value = static_cast<float>(a_control.invert ? -entry->second : entry->second);
            const auto styleSetting = !rule->settings.empty() &&
                                              rule->settings.front().operation ==
                                                  TuningUtil::FilteredLightingTemplateOperation::fogStrength ?
                                          "intFogMaxMultiplier" :
                                          "intBrightnessMultiplier";
            auto sliderDefaults = ResolveControlSliderDefaults(a_control, styleSetting);
            const auto minimum = std::min(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto maximum = std::max(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto* format = IsSafeSliderFormat(sliderDefaults.format) ? sliderDefaults.format.c_str() : "%.2f";
            if (DrawSliderWithInput(
                    a_id,
                    value,
                    minimum,
                    maximum,
                    sliderDefaults.step,
                    format,
                    sliderDefaults.width,
                    SliderInputRange::standard,
                    a_control.invert ? std::nullopt : SliderNeutralValue(styleSetting)))
            {
                entry->second = a_control.invert ? -value : value;
                TuningUtil::ApplySettings();
            }
            return true;
        }

        bool DrawFilteredBaseLightSlider(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            const auto* rule = TuningUtil::FindFilteredBaseLightRule(profile, a_control.id);
            if (!rule || rule->settings.empty()) return false;

            auto& values = TuningUtil::GetSettings(profile).filteredBaseLightAdjustments;
            auto [entry, inserted] = values.try_emplace(rule->id, rule->defaultValue);
            (void)inserted;
            auto value = static_cast<float>(a_control.invert ? -entry->second : entry->second);
            const auto styleSetting = [&]() -> std::string_view
            {
                switch (rule->settings.front().operation)
                {
                case TuningUtil::FilteredBaseLightOperation::brightness:
                    return "pointLights.fadeMultiplier";
                case TuningUtil::FilteredBaseLightOperation::sunlight:
                    return "pointLights.sunlightFadeMultiplier";
                case TuningUtil::FilteredBaseLightOperation::saturation:
                    return "pointLights.saturationMultiplier";
                case TuningUtil::FilteredBaseLightOperation::hueScale:
                    return "pointLights.hueScales.red";
                case TuningUtil::FilteredBaseLightOperation::hueShift:
                    return "pointLights.hueShift.red";
                }
                return "pointLights.fadeMultiplier";
            }();
            auto sliderDefaults = ResolveControlSliderDefaults(a_control, styleSetting);
            const auto minimum = std::min(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto maximum = std::max(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto* format = IsSafeSliderFormat(sliderDefaults.format) ? sliderDefaults.format.c_str() : "%.2f";
            if (DrawSliderWithInput(
                    a_id,
                    value,
                    minimum,
                    maximum,
                    sliderDefaults.step,
                    format,
                    sliderDefaults.width,
                    SliderInputRange::standard,
                    a_control.invert ? std::nullopt : SliderNeutralValue(styleSetting)))
            {
                entry->second = a_control.invert ? -value : value;
                ApplySliderChange(true);
            }
            return true;
        }

        const std::vector<WeatherMenuEntry>& GetWeatherMenuEntries(const std::string& a_profileName)
        {
            if (const auto existing = weatherMenuEntries.find(a_profileName); existing != weatherMenuEntries.end())
            {
                return existing->second;
            }

            auto profileName = a_profileName;
            auto weathers = WeatherPatcher::GetSelectableWeathers(profileName);
            std::ranges::sort(
                weathers,
                [](const RE::TESWeather* a_left, const RE::TESWeather* a_right)
                {
                    const auto leftName = Lowercase(WeatherPatcher::WeatherName(a_left));
                    const auto rightName = Lowercase(WeatherPatcher::WeatherName(a_right));
                    return leftName != rightName ? leftName < rightName : a_left->GetFormID() < a_right->GetFormID();
                });

            std::vector<WeatherMenuEntry> entries;
            entries.reserve(weathers.size());
            for (auto* weather : weathers)
            {
                if (!weather)
                {
                    continue;
                }
                entries.push_back({ weather, WeatherPatcher::WeatherName(weather) });
            }

            return weatherMenuEntries.emplace(a_profileName, std::move(entries)).first->second;
        }

        const std::vector<WeatherMenuEntry>& GetSliderCreatorWeatherEntries(
            const std::string& a_profileName,
            const bool a_effectLighting = false)
        {
            const auto cacheKey = a_profileName + (a_effectLighting ? ":effectLighting" : ":weather");
            if (const auto existing = sliderCreatorWeatherEntries.find(cacheKey);
                existing != sliderCreatorWeatherEntries.end())
                return existing->second;

            auto profileName = a_profileName;
            auto weathers = a_effectLighting ?
                                WeatherPatcher::GetFilterableEffectLightingWeathers(profileName) :
                                WeatherPatcher::GetFilterableWeathers(profileName);
            std::ranges::sort(
                weathers,
                [](const RE::TESWeather* a_left, const RE::TESWeather* a_right)
                {
                    const auto leftName = Lowercase(WeatherPatcher::WeatherName(a_left));
                    const auto rightName = Lowercase(WeatherPatcher::WeatherName(a_right));
                    return leftName != rightName ? leftName < rightName : a_left->GetFormID() < a_right->GetFormID();
                });

            std::vector<WeatherMenuEntry> entries;
            entries.reserve(weathers.size());
            for (auto* weather : weathers)
            {
                if (!weather) continue;
                entries.push_back({ weather, WeatherPatcher::WeatherName(weather) });
            }
            return sliderCreatorWeatherEntries.emplace(cacheKey, std::move(entries)).first->second;
        }

        const std::vector<RecordMenuEntry>& GetLightingTemplateMenuEntries()
        {
            if (lightingTemplateMenuEntries)
            {
                return *lightingTemplateMenuEntries;
            }

            std::vector<RecordMenuEntry> entries;
            if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
            {
                for (auto* lightingTemplate : dataHandler->GetFormArray<RE::BGSLightingTemplate>())
                {
                    if (lightingTemplate)
                    {
                        entries.push_back({
                            lightingTemplate,
                            RecordFilter::DisplayName(lightingTemplate),
                        });
                    }
                }
            }
            std::ranges::sort(
                entries,
                [](const RecordMenuEntry& a_left, const RecordMenuEntry& a_right)
                {
                    const auto leftName = Lowercase(a_left.label);
                    const auto rightName = Lowercase(a_right.label);
                    return leftName != rightName ? leftName < rightName :
                                                  a_left.form->GetFormID() < a_right.form->GetFormID();
                });
            lightingTemplateMenuEntries = std::move(entries);
            return *lightingTemplateMenuEntries;
        }

        const std::vector<RecordMenuEntry>& GetEffectLightingMenuEntries()
        {
            if (effectLightingMenuEntries)
            {
                return *effectLightingMenuEntries;
            }

            std::vector<RecordMenuEntry> entries;
            for (auto* weather : WeatherPatcher::GetFXWeathers())
            {
                if (weather)
                {
                    entries.push_back({
                        weather,
                        RecordFilter::DisplayName(weather),
                    });
                }
            }
            std::ranges::sort(
                entries,
                [](const RecordMenuEntry& a_left, const RecordMenuEntry& a_right)
                {
                    const auto leftName = Lowercase(a_left.label);
                    const auto rightName = Lowercase(a_right.label);
                    return leftName != rightName ? leftName < rightName :
                                                  a_left.form->GetFormID() < a_right.form->GetFormID();
                });
            effectLightingMenuEntries = std::move(entries);
            return *effectLightingMenuEntries;
        }

        const std::vector<RecordMenuEntry>& GetBaseLightMenuEntries()
        {
            if (baseLightMenuEntries) return *baseLightMenuEntries;

            std::vector<RecordMenuEntry> entries;
            if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
            {
                for (auto* light : dataHandler->GetFormArray<RE::TESObjectLIGH>())
                {
                    if (light) entries.push_back({ light, RecordFilter::DisplayName(light) });
                }
            }
            std::ranges::sort(
                entries,
                [](const RecordMenuEntry& a_left, const RecordMenuEntry& a_right)
                {
                    const auto leftName = Lowercase(a_left.label);
                    const auto rightName = Lowercase(a_right.label);
                    return leftName != rightName ? leftName < rightName :
                                                  a_left.form->GetFormID() < a_right.form->GetFormID();
                });
            baseLightMenuEntries = std::move(entries);
            return *baseLightMenuEntries;
        }

        RE::TESWeather* GetCurrentWeather()
        {
            const auto* sky = RE::Sky::GetSingleton();
            return sky ? (sky->overrideWeather ? sky->overrideWeather : sky->currentWeather) : nullptr;
        }

        std::string WeatherExclusionKey(const RE::TESWeather* a_weather)
        {
            if (!a_weather)
            {
                return {};
            }
            const auto* sourceFile = a_weather->GetFile(0);
            if (!sourceFile)
            {
                sourceFile = a_weather->GetFile();
            }
            return sourceFile ?
                       std::format("{:06X}:{}", a_weather->GetLocalFormID(), sourceFile->GetFilename()) :
                       std::format("{:08X}", a_weather->GetFormID());
        }

        std::string WeatherDisplayLabel(const RE::TESWeather* a_weather)
        {
            return a_weather ? WeatherPatcher::WeatherName(a_weather) : DisplayText("weatherUnavailableLabel");
        }

        std::filesystem::path QuickSelectListPath(const std::string& a_profileName)
        {
            return kQuickSelectRoot / TuningUtil::ProfileDirectory(a_profileName).filename() /
                   kQuickSelectListFileName;
        }

        bool DeleteGeneratedUserSettings()
        {
            constexpr std::array generatedFiles{
                std::string_view{ "userSettings.json" },
                kQuickSelectListFileName,
            };
            std::size_t deletedFiles = 0;
            std::size_t disabledPresets = 0;
            bool success = true;
            std::string presetError;
            if (!WeatherPatcher::DisableAllAutoLoadPresets(disabledPresets, presetError))
            {
                logger::warn("[Tuning Menu] preset auto-load disable failed | {}", presetError);
                success = false;
            }
            std::error_code rootError;
            if (std::filesystem::is_directory(kQuickSelectRoot, rootError))
            {
                std::error_code iteratorError;
                const auto options = std::filesystem::directory_options::skip_permission_denied;
                for (std::filesystem::directory_iterator iterator(kQuickSelectRoot, options, iteratorError), end;
                    iterator != end && !iteratorError;
                    iterator.increment(iteratorError))
                {
                    if (!iterator->is_directory())
                    {
                        continue;
                    }
                    for (const auto fileName : generatedFiles)
                    {
                        const auto path = iterator->path() / fileName;
                        std::error_code removeError;
                        if (std::filesystem::remove(path, removeError))
                        {
                            ++deletedFiles;
                        }
                        else if (removeError)
                        {
                            logger::warn("[Tuning Menu] delete failed | path={} | {}", path.string(), removeError.message());
                            success = false;
                        }
                    }
                }
                if (iteratorError)
                {
                    logger::warn(
                        "[Tuning Menu] user settings enumeration failed | path={} | {}",
                        kQuickSelectRoot.string(),
                        iteratorError.message());
                    success = false;
                }
            }
            else if (rootError)
            {
                logger::warn("[Tuning Menu] access failed | path={} | {}", kQuickSelectRoot.string(), rootError.message());
                success = false;
            }

            quickWeatherSelections.clear();
            profilePriorityInputs.clear();
            presetVisualStates.clear();
            TuningUtil::InvalidateDiscoveryCaches();
            TuningUtil::ApplySettings();
            logger::info(
                "[Tuning Menu] reset | userFiles={} | autoLoadPresets={}",
                deletedFiles,
                disabledPresets);
            return success;
        }

        bool SaveQuickWeatherSelections(
            const std::string& a_profileName,
            const std::vector<RE::TESWeather*>& a_selections)
        {
            QuickSelectList list;
            list.weathers.reserve(a_selections.size());
            for (const auto* weather : a_selections)
            {
                if (auto key = WeatherExclusionKey(weather); !key.empty())
                {
                    list.weathers.push_back(std::move(key));
                }
            }

            const auto path = QuickSelectListPath(a_profileName);
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                logger::warn("[Tuning Menu] Quick Select directory create failed | path={} | {}", path.parent_path().string(), error.message());
                return false;
            }

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << rfl::json::write(list, rfl::json::pretty) << '\n';
            if (!file)
            {
                logger::warn("[Tuning Menu] Quick Select save failed | path={}", path.string());
                return false;
            }
            return true;
        }

        std::vector<RE::TESWeather*>& GetQuickWeatherSelections(
            const std::string& a_profileName,
            const std::vector<WeatherMenuEntry>& a_entries)
        {
            if (const auto existing = quickWeatherSelections.find(a_profileName);
                existing != quickWeatherSelections.end())
            {
                return existing->second;
            }

            std::vector<RE::TESWeather*> selections;
            const auto path = QuickSelectListPath(a_profileName);
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return quickWeatherSelections.emplace(a_profileName, std::move(selections)).first->second;
            }

            const std::string text(std::istreambuf_iterator<char>(file), {});
            const auto parsed = rfl::json::read<QuickSelectList, rfl::DefaultIfMissing>(text);
            if (!parsed)
            {
                logger::warn("[Tuning Menu] Quick Select load failed | path={} | {}", path.string(), parsed.error().what());
                return quickWeatherSelections.emplace(a_profileName, std::move(selections)).first->second;
            }

            auto pruned = false;
            std::unordered_set<RE::FormID> seen;
            for (const auto& key : parsed.value().weathers)
            {
                auto* weather = Config::LiteForm::FromString(key).Get<RE::TESWeather>();
                const auto selectable = weather &&
                                        std::ranges::find(a_entries, weather, &WeatherMenuEntry::weather) != a_entries.end();
                if (!selectable || !seen.insert(weather->GetFormID()).second)
                {
                    pruned = true;
                    continue;
                }
                selections.push_back(weather);
            }

            auto& result = quickWeatherSelections.emplace(a_profileName, std::move(selections)).first->second;
            if (pruned)
            {
                SaveQuickWeatherSelections(a_profileName, result);
            }
            return result;
        }

        std::string GetCurrentRegion()
        {
            const auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            auto* stat = Config::StatData::GetSingleton();
            if (!stat->mmsfAPI) stat->mmsfAPI = API::MMSF::RequestMMSFAPI();
            const auto region = RegionRuntime::GetRegion(stat->mmsfAPI, cell);
            return region.empty() ? DisplayText("emptyList") : region;
        }

        void DrawCurrentRegion(const MenuControl& a_control)
        {
            const auto label = a_control.label.empty() ? "Current Region" : a_control.label;
            ImGuiMCP::TextWrapped("%s: %s", label.c_str(), GetCurrentRegion().c_str());
        }

        void DrawCurrentWeather(const MenuControl& a_control)
        {
            const auto label = a_control.label.empty() ? "Current Weather" : a_control.label;
            const auto* sky = RE::Sky::GetSingleton();
            const auto weather = WeatherDisplayLabel(GetCurrentWeather());
            ImGuiMCP::TextUnformatted((label + ":").c_str());
            ImGuiMCP::SameLine();
            ImGuiMCP::PushStyleColor(
                ImGuiMCP::ImGuiCol_Text,
                ImGuiMCP::ImVec4(1.0f, 0.647f, 0.0f, 1.0f));
            ImGuiMCP::TextUnformatted(weather.c_str());
            ImGuiMCP::PopStyleColor();
            if (sky && sky->overrideWeather)
            {
                ImGuiMCP::SameLine(0.0f, 0.0f);
                ImGuiMCP::PushStyleColor(
                    ImGuiMCP::ImGuiCol_Text,
                    ImGuiMCP::ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                ImGuiMCP::TextUnformatted(DisplayText("weatherOverrideSuffix").c_str());
                ImGuiMCP::PopStyleColor();
            }
        }

        void SelectWeather(const std::string& a_profile, RE::TESWeather* a_weather)
        {
            if (!a_weather)
            {
                return;
            }

            auto profile = a_profile;
            const auto& settings = TuningUtil::GetSettings(profile);
            const auto lockEnabled =
                settings.EnableProfile && weatherLockPreferences[profile] &&
                menuFrameworkOpen.load(std::memory_order_acquire);
            WeatherLock::SetSelectedWeather(a_weather);
            WeatherLock::SetEnabled(lockEnabled);
            WeatherRuntime::SetWeatherInstant(
                a_weather,
                lockEnabled);
            statusMessage.clear();
        }

        void DrawWeatherLockToggle(
            const MenuDefinition& a_menu,
            const std::string& a_id);

        void DrawWeatherSelectWindow(
            const MenuDefinition& a_menu,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            const auto& entries = GetWeatherMenuEntries(profile);
            if (entries.empty())
            {
                DrawDisplayText("noSelectableWeathers");
                return;
            }

            auto* selectedWeather = WeatherLock::GetSelectedWeather();
            if (!selectedWeather) selectedWeather = GetCurrentWeather();

            const auto listID = "##WeatherSelect" + a_id;
            if (ImGuiMCP::BeginListBox(listID.c_str(), ImGuiMCP::ImVec2(0.0f, kWeatherSelectWindowLength)))
            {
                for (const auto& entry : entries)
                {
                    const auto selected = entry.weather == selectedWeather;
                    const auto itemLabel = entry.label + "##WeatherSelect" + profile +
                                           std::format("{:08X}", entry.weather->GetFormID());
                    if (ImGuiMCP::Selectable(itemLabel.c_str(), selected))
                    {
                        SelectWeather(profile, entry.weather);
                        selectedWeather = entry.weather;
                    }
                    if (selected) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndListBox();
            }
        }

        void DrawQuickSelect(
            const MenuDefinition& a_menu,
            const std::string& a_id,
            const bool a_showControls)
        {
            auto profile = a_menu.profile;
            const auto& entries = GetWeatherMenuEntries(profile);
            if (entries.empty())
            {
                DrawDisplayText("noSelectableWeathers");
                return;
            }

            auto* selectedWeather = WeatherLock::GetSelectedWeather();
            if (!selectedWeather)
            {
                selectedWeather = GetCurrentWeather();
            }

            auto& quickSelections = GetQuickWeatherSelections(profile, entries);
            if (std::erase_if(
                    quickSelections,
                    [&](RE::TESWeather* a_weather)
                    {
                        return std::ranges::find(entries, a_weather, &WeatherMenuEntry::weather) == entries.end();
                    }) > 0)
            {
                SaveQuickWeatherSelections(profile, quickSelections);
            }

            if (a_showControls)
            {
                const auto quickCandidate =
                    std::ranges::find(
                        entries,
                        selectedWeather,
                        &WeatherMenuEntry::weather);
                const auto addButtonId =
                    SKSEMenuSettings::Label(
                        "addCurrentQuickSelect",
                        "Add Current to Quick Select") +
                    "##" + a_id;
                ImGuiMCP::BeginDisabled(quickCandidate == entries.end());
                if (ImGuiMCP::Button(addButtonId.c_str()) &&
                    !std::ranges::contains(
                        quickSelections,
                        quickCandidate->weather))
                {
                    quickSelections.push_back(quickCandidate->weather);
                    if (!SaveQuickWeatherSelections(profile, quickSelections))
                    {
                        statusMessage = StatusText("quickSelectSaveFailure");
                    }
                }
                ImGuiMCP::EndDisabled();

                SameActionLine();
                const ButtonColorStyle destructiveColor(
                    SKSEMenuSettings::GetButtonColor(
                        SKSEMenuSettings::ButtonKind::destructive));
                const auto removeCandidate =
                    std::ranges::find(quickSelections, selectedWeather);
                const auto removeButtonId =
                    SKSEMenuSettings::Label(
                        "removeSelectedQuickSelect",
                        "Remove") +
                    "##" + a_id;
                ImGuiMCP::BeginDisabled(
                    removeCandidate == quickSelections.end());
                if (ImGuiMCP::Button(removeButtonId.c_str()))
                {
                    quickSelections.erase(removeCandidate);
                    if (!SaveQuickWeatherSelections(profile, quickSelections))
                    {
                        statusMessage = StatusText("quickSelectSaveFailure");
                    }
                }
                ImGuiMCP::EndDisabled();

                SameActionLine();
                const auto clearButtonId =
                    SKSEMenuSettings::Label("clearQuickSelect", "Clear All") +
                    "##" + a_id;
                ImGuiMCP::BeginDisabled(quickSelections.empty());
                if (ImGuiMCP::Button(clearButtonId.c_str()))
                {
                    quickSelections.clear();
                    if (!SaveQuickWeatherSelections(profile, quickSelections))
                    {
                        statusMessage =
                            StatusText("quickSelectClearSaveFailure");
                    }
                }
                ImGuiMCP::EndDisabled();
            }

            const auto quickListId = "##QuickSelect" + a_id;
            if (ImGuiMCP::BeginListBox(quickListId.c_str(), ImGuiMCP::ImVec2(0.0f, kQuickSelectWindowLength)))
            {
                if (quickSelections.empty())
                {
                    DrawDisplayText("noQuickSelectWeathers", true);
                }
                else
                {
                    for (auto* quickWeather : quickSelections)
                    {
                        const auto entry = std::ranges::find(entries, quickWeather, &WeatherMenuEntry::weather);
                        if (entry == entries.end())
                        {
                            continue;
                        }
                        const auto itemLabel = entry->label + "##Quick" + profile +
                                               std::format("{:08X}", entry->weather->GetFormID());
                        if (ImGuiMCP::Selectable(itemLabel.c_str(), entry->weather == selectedWeather))
                        {
                            SelectWeather(profile, entry->weather);
                            selectedWeather = entry->weather;
                        }
                    }
                }
                ImGuiMCP::EndListBox();
            }
        }

        void DrawQuickSelectSection(
            const MenuDefinition& a_menu,
            const std::string& a_id,
            const float a_width,
            const bool a_showControls)
        {
            constexpr auto flags = ImGuiMCP::ImGuiChildFlags_AutoResizeY |
                                   ImGuiMCP::ImGuiChildFlags_AlwaysAutoResize;
            const auto visible = ImGuiMCP::BeginChild(
                ("QuickSelectSection##" + a_id).c_str(),
                ImGuiMCP::ImVec2(a_width, 0.0f),
                flags);
            if (visible)
            {
                const auto label = SKSEMenuSettings::Label("quickSelect", "Quick Select") +
                                   "##" + a_id;
                if (ImGuiMCP::CollapsingHeader(label.c_str()))
                    DrawQuickSelect(a_menu, a_id, a_showControls);
            }
            ImGuiMCP::EndChild();
        }

        float DrawTimeOfDayControls(const MenuDefinition& a_menu, const std::string& a_id);

        void DrawWeatherControlCompact(const MenuDefinition& a_menu, const std::string& a_id)
        {
            const auto controlWidth = DrawTimeOfDayControls(a_menu, a_id + "Time");
            DrawQuickSelectSection(a_menu, a_id + "QuickSelect", controlWidth, false);
        }

        void DrawWeatherSelector(const MenuDefinition& a_menu, const MenuControl& a_control, const std::string& a_id)
        {
            const auto drawBox =
                [&](const std::string_view a_suffix,
                    const std::string_view a_title,
                    auto&& a_draw)
            {
                constexpr auto flags =
                    ImGuiMCP::ImGuiChildFlags_Border |
                    ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                    ImGuiMCP::ImGuiChildFlags_AutoResizeY;
                const auto padding = SKSEMenuSettings::GetBoxPadding();
                const auto customPadding =
                    padding[0] > 0.0f || padding[1] > 0.0f;
                if (customPadding)
                {
                    ImGuiMCP::PushStyleVar(
                        ImGuiMCP::ImGuiStyleVar_WindowPadding,
                        ImGuiMCP::ImVec2(padding[0], padding[1]));
                }
                const auto visible = ImGuiMCP::BeginChild(
                    (a_id + std::string(a_suffix)).c_str(),
                    ImGuiMCP::ImVec2(0.0f, 0.0f),
                    flags);
                if (visible)
                {
                    if (!a_title.empty()) DrawHeader(a_title);
                    a_draw();
                }
                ImGuiMCP::EndChild();
                if (customPadding)
                {
                    ImGuiMCP::PopStyleVar();
                }
            };

            const auto controlWidth = DrawTimeOfDayControls(a_menu, a_id);
            DrawQuickSelectSection(a_menu, a_id + "QuickSelect", controlWidth, true);
            ImGuiMCP::Spacing();
            drawBox(
                "WeatherSelectBox",
                "Weather Select",
                [&]
                {
                    DrawCurrentWeather(a_control);
                    DrawWeatherSelectWindow(
                        a_menu,
                        a_id + "Window");
                });
            ImGuiMCP::Separator();
            DrawCurrentRegion(a_control);
            ImGuiMCP::Separator();
        }

        void ActivateWeatherLockPreference(const MenuDefinition& a_menu)
        {
            if (!menuFrameworkOpen.load(std::memory_order_acquire) ||
                (!HasModuleKind(a_menu, "weatherSelector") &&
                    !HasModuleKind(a_menu, "weatherControlCompact")) ||
                a_menu.profile.empty() ||
                activeWeatherLockProfile == a_menu.profile)
            {
                return;
            }

            activeWeatherLockProfile = a_menu.profile;
            auto profile = a_menu.profile;
            const auto& settings = TuningUtil::GetSettings(profile);
            auto* selectedWeather = WeatherLock::GetSelectedWeather();
            if (!selectedWeather)
            {
                selectedWeather = GetCurrentWeather();
                WeatherLock::SetSelectedWeather(selectedWeather);
            }

            const auto enable =
                settings.EnableProfile && weatherLockPreferences[profile] && selectedWeather;
            WeatherLock::SetEnabled(enable);
            if (enable)
            {
                WeatherRuntime::SetWeatherInstant(
                    selectedWeather,
                    true);
            }
        }

        void DrawWeatherLockToggle(
            const MenuDefinition& a_menu,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto lockEnabled = weatherLockPreferences[profile];
            if (!ImGuiMCP::Checkbox(a_id.c_str(), &lockEnabled))
            {
                return;
            }

            weatherLockPreferences[profile] = lockEnabled;
            auto* selectedWeather = WeatherLock::GetSelectedWeather();
            if (lockEnabled && !selectedWeather)
            {
                selectedWeather = GetCurrentWeather();
                WeatherLock::SetSelectedWeather(selectedWeather);
            }

            const auto enable = menuFrameworkOpen.load(std::memory_order_acquire) &&
                                settings.EnableProfile && lockEnabled && selectedWeather;
            WeatherLock::SetEnabled(enable);
            if (enable)
            {
                WeatherRuntime::SetWeatherInstant(
                    selectedWeather,
                    true);
                statusMessage = StatusText(
                    "weatherLockEnabledSession",
                    { { "weather", WeatherPatcher::WeatherName(selectedWeather) } });
            }
            else if (lockEnabled)
            {
                statusMessage = StatusText("weatherLockUnavailable");
            }
            else
            {
                WeatherLock::ReleaseOverride();
                statusMessage = StatusText("weatherLockDisabledReleaseRequested");
            }
        }

        void DrawProfileToggle(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto enabled = settings.EnableProfile;

            if (!ImGuiMCP::Checkbox(a_id.c_str(), &enabled))
            {
                return;
            }

            const auto previous = settings.EnableProfile;
            settings.EnableProfile = enabled;
            const std::vector<std::string> scope{ "EnableProfile" };
            if (!TuningUtil::SavePageSettings(profile, scope))
            {
                settings.EnableProfile = previous;
                statusMessage = StatusText("profileStateSaveFailure");
                return;
            }

            TuningUtil::ApplySettings();
            if (HasWeatherMenuControls(a_menu))
            {
                weatherMenuEntries.clear();
                sliderCreatorWeatherEntries.clear();
                activeWeatherLockProfile.clear();
                ActivateWeatherLockPreference(a_menu);
            }
            statusMessage = StatusText(enabled ? "profileEnabled" : "profileDisabled");
        }

        float DrawTimeOfDayControls(const MenuDefinition& a_menu, const std::string& a_id)
        {
            constexpr auto inputWidth = 80.0f;
            constexpr auto fallbackFormat = "%.2f";
            constexpr auto maximumGameHour = 23.99f;
            const auto style = SKSEMenuSettings::ResolveSliderDefaults(
                "timeOfDay",
                0.0f,
                0.0f,
                fallbackFormat);
            const auto* format =
                IsSafeSliderFormat(style.format) ?
                    style.format.c_str() :
                    fallbackFormat;
            const auto sliderWidth = style.width != 0.0f ? style.width : ImGuiMCP::CalcItemWidth();

            ImGuiMCP::TextUnformatted("Time of Day");
            ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(inputWidth);

            auto* calendar = RE::Calendar::GetSingleton();
            auto changed = false;
            auto value = calendar && calendar->gameHour ? calendar->GetHour() : 0.0f;
            if (!calendar || !calendar->gameHour)
            {
                ImGuiMCP::TextDisabled(
                    "%s",
                    DisplayText("currentTimeUnavailable").c_str());
            }
            else
            {
                auto inputValue = value;
                if (ImGuiMCP::InputFloat(
                        ("##TimeOfDayValue" + a_id).c_str(),
                        &inputValue,
                        0.0f,
                        0.0f,
                        format))
                {
                    value = std::clamp(inputValue, 0.0f, maximumGameHour);
                    changed = true;
                }
            }

            const auto* imguiStyle = ImGuiMCP::GetStyle();
            const auto lockSpacing = imguiStyle ? imguiStyle->ItemSpacing.x * 2.0f : 16.0f;
            ImGuiMCP::SameLine(0.0f, lockSpacing);
            ImGuiMCP::TextUnformatted(SKSEMenuSettings::Label("lockWeather", "Lock Weather").c_str());
            ImGuiMCP::SameLine();
            DrawWeatherLockToggle(a_menu, "##LockWeather" + a_id);

            auto sliderValue = std::clamp(value, 0.0f, maximumGameHour);
            ImGuiMCP::SetNextItemWidth(sliderWidth);
            if (calendar && calendar->gameHour && ImGuiMCP::SliderFloat(
                    ("##TimeOfDaySlider" + a_id).c_str(),
                    &sliderValue,
                    0.0f,
                    maximumGameHour,
                    ""))
            {
                value = SnapSliderValue(
                    sliderValue,
                    0.0f,
                    maximumGameHour,
                    style.step);
                changed = true;
            }
            if (changed)
            {
                calendar->gameHour->value = std::clamp(value, 0.0f, maximumGameHour);
            }
            return sliderWidth;
        }

        struct LinkableParts
        {
            bool linked = false;
            std::string link;
            double scale = 1.0;
        };

        LinkableParts ReadLinkable(const WeatherPatcher::SettingLink& a_value)
        {
            LinkableParts result;
            if (a_value)
            {
                result.linked = true;
                result.link = std::get<0>(*a_value);
                result.scale = std::get<1>(*a_value);
            }
            return result;
        }

        std::vector<double> ResolveLinkableValues(
            const std::span<const NamedLinkable> a_fields,
            const double a_linkNeutral,
            const std::span<const double> a_directValues = {})
        {
            std::vector<double> result(a_fields.size(), a_linkNeutral);
            std::vector<std::uint8_t> state(a_fields.size());
            std::function<double(std::size_t)> resolve = [&](const std::size_t a_index)
            {
                if (state[a_index] == 2)
                {
                    return result[a_index];
                }
                if (state[a_index] == 1)
                {
                    return a_linkNeutral;
                }
                state[a_index] = 1;
                const auto parts = ReadLinkable(*a_fields[a_index].value);
                if (!parts.linked)
                {
                    result[a_index] = a_directValues.empty() ?
                                          a_fields[a_index].direct ? *a_fields[a_index].direct : a_linkNeutral :
                                          a_directValues[a_index];
                }
                else if (const auto target = std::ranges::find(a_fields, parts.link, &NamedLinkable::key);
                    target != a_fields.end())
                {
                    const auto targetValue = resolve(static_cast<std::size_t>(std::distance(a_fields.begin(), target)));
                    result[a_index] = a_linkNeutral + ((targetValue - a_linkNeutral) * parts.scale);
                }
                state[a_index] = 2;
                return result[a_index];
            };
            for (std::size_t index = 0; index < a_fields.size(); ++index)
            {
                resolve(index);
            }
            return result;
        }

        bool LinkWouldCycle(
            const std::span<const NamedLinkable> a_fields,
            const std::size_t a_source,
            std::size_t a_target)
        {
            std::vector<bool> visited(a_fields.size());
            while (a_target < a_fields.size() && !visited[a_target])
            {
                if (a_target == a_source)
                {
                    return true;
                }
                visited[a_target] = true;
                const auto parts = ReadLinkable(*a_fields[a_target].value);
                if (!parts.linked)
                {
                    return false;
                }
                const auto next = std::ranges::find(a_fields, parts.link, &NamedLinkable::key);
                if (next == a_fields.end())
                {
                    return false;
                }
                a_target = static_cast<std::size_t>(std::distance(a_fields.begin(), next));
            }
            return true;
        }

        bool DrawLinkableSetting(
            const std::span<const NamedLinkable> a_fields,
            const std::span<const double> a_resolved,
            const std::size_t a_index,
            const float a_minimum,
            const float a_maximum,
            const std::string& a_idPrefix,
            const float a_step = 0.0f,
            const float a_scaleMaximum = 4.0f,
            const std::string_view a_label = {},
            const MenuControl* a_styleControl = nullptr,
            double* a_directOverride = nullptr,
            const double a_unlinkedValue = 0.0,
            const bool a_linksOnly = false,
            const std::string_view a_catalogPrefix = {})
        {
            const auto& field = a_fields[a_index];
            auto parts = ReadLinkable(*field.value);
            ImGuiMCP::TextUnformatted(a_label.empty() ? field.label.data() : a_label.data());

            auto changed = false;
            if (a_linksOnly)
            {
                auto linked = parts.linked;
                const auto linkId = "Link##" + a_idPrefix + std::string(field.key);
                if (ImGuiMCP::Checkbox(linkId.c_str(), &linked))
                {
                    if (linked)
                    {
                        const auto target = std::ranges::find_if(
                            a_fields,
                            [&](const NamedLinkable& a_candidate)
                            {
                                const auto candidateIndex = static_cast<std::size_t>(&a_candidate - a_fields.data());
                                return candidateIndex != a_index && !LinkWouldCycle(a_fields, a_index, candidateIndex);
                            });
                        if (target != a_fields.end())
                        {
                            parts.linked = true;
                            parts.link = std::string(target->key);
                            parts.scale = 1.0;
                            *field.value = std::tuple{ parts.link, parts.scale };
                            changed = true;
                        }
                    }
                    else
                    {
                        parts.linked = false;
                        if (a_directOverride) *a_directOverride = a_resolved[a_index];
                        else if (field.direct) *field.direct = a_resolved[a_index];
                        field.value->reset();
                        changed = true;
                    }
                }

                parts = ReadLinkable(*field.value);
                const auto sourceId = "Link##" + a_idPrefix + std::string(field.key) + "Source";
                ImGuiMCP::BeginDisabled(!parts.linked);
                if (ImGuiMCP::BeginCombo(sourceId.c_str(), parts.linked ? parts.link.c_str() : "Not linked"))
                {
                    for (std::size_t targetIndex = 0; targetIndex < a_fields.size(); ++targetIndex)
                    {
                        if (targetIndex == a_index || LinkWouldCycle(a_fields, a_index, targetIndex))
                        {
                            continue;
                        }
                        const auto& target = a_fields[targetIndex];
                        if (ImGuiMCP::Selectable(target.label.data(), parts.link == target.key))
                        {
                            parts.link = std::string(target.key);
                            *field.value = std::tuple{ parts.link, parts.scale };
                            changed = true;
                        }
                    }
                    ImGuiMCP::EndCombo();
                }
                ImGuiMCP::EndDisabled();

                auto scale = static_cast<float>(parts.scale);
                const auto scaleId = "Scale##" + a_idPrefix + std::string(field.key);
                auto scaleStyle = SKSEMenuSettings::ResolveSliderDefaults("linkScale", 0.0f, 0.1f, "%.1f");
                if (a_styleControl && std::isfinite(a_styleControl->width)) scaleStyle.width = a_styleControl->width;
                ImGuiMCP::BeginDisabled(!parts.linked);
                const auto* scaleFormat = IsSafeSliderFormat(scaleStyle.format) ? scaleStyle.format.c_str() : "%.1f";
                if (DrawSliderWithInput(
                        scaleId,
                        scale,
                        0.0f,
                        a_scaleMaximum,
                        scaleStyle.step,
                        scaleFormat,
                        scaleStyle.width))
                {
                    *field.value = std::tuple{ parts.link, static_cast<double>(scale) };
                    changed = true;
                }
                ImGuiMCP::EndDisabled();
            }
            else
            {
                auto directValue = static_cast<float>(
                    parts.linked     ? a_resolved[a_index] :
                    a_directOverride ? *a_directOverride :
                    field.direct     ? *field.direct :
                                       a_unlinkedValue);
                const auto valueId = "##Value" + a_idPrefix + std::string(field.key);
                const auto fallbackFormat = a_step >= 1.0f ? "%.0f" : a_step > 0.0f ? "%.1f" :
                                                                                      "%.2f";
                auto valueStyle = SKSEMenuSettings::ResolveSliderDefaults(
                    a_styleControl ? a_styleControl->setting : a_idPrefix + std::string(field.key),
                    0.0f,
                    a_step,
                    fallbackFormat);
                if (a_styleControl)
                {
                    if (std::isfinite(a_styleControl->width)) valueStyle.width = a_styleControl->width;
                    if (std::isfinite(a_styleControl->step)) valueStyle.step = a_styleControl->step;
                    if (!a_styleControl->format.empty()) valueStyle.format = a_styleControl->format;
                }
                ImGuiMCP::BeginDisabled(parts.linked);
                const auto* valueFormat = IsSafeSliderFormat(valueStyle.format) ? valueStyle.format.c_str() : fallbackFormat;
                const auto neutralValue = a_styleControl ?
                                              ControlNeutralValue(*a_styleControl) :
                                          a_catalogPrefix.empty() ?
                                              std::nullopt :
                                              SliderNeutralValue(
                                                  std::string(a_catalogPrefix) + "." +
                                                  std::string(field.key));
                if (DrawSliderWithInput(
                        valueId,
                        directValue,
                        a_minimum,
                        a_maximum,
                        valueStyle.step,
                        valueFormat,
                        valueStyle.width,
                        SliderInputRange::standard,
                        neutralValue))
                {
                    if (a_directOverride) *a_directOverride = directValue;
                    else if (field.direct) *field.direct = static_cast<double>(directValue);
                    changed = true;
                }
                ImGuiMCP::EndDisabled();
            }
            ImGuiMCP::Separator();
            return changed;
        }

        bool DrawLinkableCategory(
            const std::span<const NamedLinkable> a_fields,
            const float a_minimum,
            const float a_maximum,
            const std::string& a_idPrefix,
            const float a_step = 0.0f,
            const double a_linkNeutral = 0.0,
            const float a_scaleMaximum = 4.0f,
            const bool a_linksOnly = false,
            const std::string_view a_catalogPrefix = {})
        {
            auto changed = false;
            const auto resolved = ResolveLinkableValues(a_fields, a_linkNeutral);
            for (std::size_t index = 0; index < a_fields.size(); ++index)
            {
                changed |= DrawLinkableSetting(
                    a_fields,
                    resolved,
                    index,
                    a_minimum,
                    a_maximum,
                    a_idPrefix,
                    a_step,
                    a_scaleMaximum,
                    {},
                    nullptr,
                    nullptr,
                    a_linkNeutral,
                    a_linksOnly,
                    a_catalogPrefix);
            }
            return changed;
        }

        bool DrawFilteredLinkableCategory(
            const std::string& a_profile,
            const std::span<const NamedLinkable> a_fields,
            const std::string_view a_controlID,
            const float a_minimum,
            const float a_maximum,
            const std::string& a_idPrefix,
            const float a_step,
            const double a_neutral,
            const float a_scaleMaximum,
            const std::string_view a_catalogPrefix)
        {
            auto profile = a_profile;
            auto& adjustments = TuningUtil::GetSettings(profile).filteredWeatherAdjustments;
            std::vector<double*> direct;
            std::vector<double> values;
            direct.reserve(a_fields.size());
            values.reserve(a_fields.size());
            for (const auto& field : a_fields)
            {
                const auto id = std::string(a_controlID) + "_" + Lowercase(std::string(field.key));
                const auto* rule = TuningUtil::FindFilteredWeatherRule(a_profile, id);
                if (!rule)
                {
                    return false;
                }
                auto [entry, inserted] = adjustments.try_emplace(id, rule->defaultValue);
                (void) inserted;
                direct.push_back(std::addressof(entry->second));
                values.push_back(entry->second);
            }

            const auto resolved = ResolveLinkableValues(a_fields, a_neutral, values);
            auto changed = false;
            for (std::size_t index = 0; index < a_fields.size(); ++index)
            {
                changed |= DrawLinkableSetting(
                    a_fields,
                    resolved,
                    index,
                    a_minimum,
                    a_maximum,
                    a_idPrefix,
                    a_step,
                    a_scaleMaximum,
                    {},
                    nullptr,
                    direct[index],
                    a_neutral,
                    false,
                    a_catalogPrefix);
            }
            return changed;
        }

        bool DrawValueOnlyCategory(
            const std::span<const NamedValue> a_fields,
            const float a_minimum,
            const float a_maximum,
            const std::string& a_idPrefix,
            const float a_step = 0.0f,
            const std::string_view a_catalogPrefix = {})
        {
            auto changed = false;
            for (const auto& field : a_fields)
            {
                ImGuiMCP::TextUnformatted(field.label.data());

                auto value = static_cast<float>(*field.value);
                const auto valueId = "##Value" + a_idPrefix + std::string(field.key);
                const auto fallbackFormat = a_step >= 1.0f ? "%.0f" : a_step > 0.0f ? "%.1f" :
                                                                                      "%.2f";
                const auto valueStyle = SKSEMenuSettings::ResolveSliderDefaults(
                    a_idPrefix + std::string(field.key),
                    0.0f,
                    a_step,
                    fallbackFormat);
                const auto* valueFormat = IsSafeSliderFormat(valueStyle.format) ? valueStyle.format.c_str() : fallbackFormat;
                const auto neutralValue = a_catalogPrefix.empty() ?
                                              std::nullopt :
                                              SliderNeutralValue(
                                                  std::string(a_catalogPrefix) + "." +
                                                  std::string(field.key));
                if (DrawSliderWithInput(
                        valueId,
                        value,
                        a_minimum,
                        a_maximum,
                        valueStyle.step,
                        valueFormat,
                        valueStyle.width,
                        SliderInputRange::standard,
                        neutralValue))
                {
                    *field.value = static_cast<double>(value);
                    changed = true;
                }
                ImGuiMCP::Separator();
            }
            return changed;
        }

        struct HueShiftParts
        {
            bool linked = false;
            std::string link;
            double scale = 1.0;
        };

        HueShiftParts ReadHueShift(const WeatherPatcher::SettingLink& a_value)
        {
            HueShiftParts result;
            if (a_value)
            {
                result.linked = true;
                result.link = std::get<0>(*a_value);
                result.scale = std::get<1>(*a_value);
            }
            return result;
        }

        WeatherPatcher::HueShiftBands ScaleHueShift(WeatherPatcher::HueShiftBands a_value, const double a_scale)
        {
            a_value.red *= a_scale;
            a_value.orange *= a_scale;
            a_value.yellow *= a_scale;
            a_value.green *= a_scale;
            a_value.teal *= a_scale;
            a_value.blue *= a_scale;
            a_value.magenta *= a_scale;
            return a_value;
        }

        std::vector<WeatherPatcher::HueShiftBands> ResolveHueShiftValues(
            const std::span<const NamedHueShift> a_fields,
            const std::span<const WeatherPatcher::HueShiftBands> a_directValues = {})
        {
            std::vector<WeatherPatcher::HueShiftBands> result(a_fields.size());
            std::vector<std::uint8_t> state(a_fields.size());
            std::function<WeatherPatcher::HueShiftBands(std::size_t)> resolve = [&](const std::size_t a_index)
            {
                if (state[a_index] == 2)
                {
                    return result[a_index];
                }
                if (state[a_index] == 1)
                {
                    return WeatherPatcher::HueShiftBands{};
                }
                state[a_index] = 1;
                const auto parts = ReadHueShift(*a_fields[a_index].value);
                result[a_index] = a_directValues.empty() ? *a_fields[a_index].direct : a_directValues[a_index];
                if (parts.linked)
                {
                    if (const auto target = std::ranges::find(a_fields, parts.link, &NamedHueShift::key);
                        target != a_fields.end())
                    {
                        result[a_index] = ScaleHueShift(
                            resolve(static_cast<std::size_t>(std::distance(a_fields.begin(), target))),
                            parts.scale);
                    }
                }
                state[a_index] = 2;
                return result[a_index];
            };
            for (std::size_t index = 0; index < a_fields.size(); ++index)
            {
                resolve(index);
            }
            return result;
        }

        bool DrawHueShiftCategory(
            const std::span<const NamedHueShift> a_fields,
            const std::string& a_idPrefix,
            std::vector<WeatherPatcher::HueShiftBands>* a_directOverrides = nullptr)
        {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 7> bands{
                std::pair{ "red", "Red" },
                std::pair{ "orange", "Orange" },
                std::pair{ "yellow", "Yellow" },
                std::pair{ "green", "Green" },
                std::pair{ "teal", "Teal" },
                std::pair{ "blue", "Blue" },
                std::pair{ "magenta", "Magenta" },
            };
            auto changed = false;
            const auto resolved = a_directOverrides ?
                                      ResolveHueShiftValues(a_fields, *a_directOverrides) :
                                      ResolveHueShiftValues(a_fields);
            for (std::size_t index = 0; index < a_fields.size(); ++index)
            {
                const auto& field = a_fields[index];
                auto parts = ReadHueShift(*field.value);
                ImGuiMCP::TextUnformatted(field.label.data());
                auto direct = parts.linked ?
                                  resolved[index] :
                              a_directOverrides ? (*a_directOverrides)[index] :
                                                  *field.direct;
                std::array<double*, 7> values{
                    &direct.red, &direct.orange, &direct.yellow, &direct.green,
                    &direct.teal, &direct.blue, &direct.magenta
                };
                ImGuiMCP::BeginDisabled(parts.linked);
                for (std::size_t band = 0; band < bands.size(); ++band)
                {
                    auto value = static_cast<float>(*values[band]);
                    const auto valueId = std::string(bands[band].second) + "##" + a_idPrefix + std::string(field.key) + std::string(bands[band].first);
                    const auto valueStyle = SKSEMenuSettings::ResolveSliderDefaults("hueShift", 0.0f, 0.1f, "%.1f");
                    const auto* valueFormat = IsSafeSliderFormat(valueStyle.format) ? valueStyle.format.c_str() : "%.1f";
                    if (DrawSliderWithInput(
                            valueId,
                            value,
                            -180.0f,
                            180.0f,
                            valueStyle.step,
                            valueFormat,
                            valueStyle.width))
                    {
                        *values[band] = value;
                        if (a_directOverrides) (*a_directOverrides)[index] = direct;
                        else *field.direct = direct;
                        changed = true;
                    }
                }
                ImGuiMCP::EndDisabled();
                ImGuiMCP::Separator();
            }
            return changed;
        }

        bool DrawFilteredHueShiftCategory(
            const std::string& a_profile,
            const std::span<const NamedHueShift> a_fields,
            const std::string_view a_controlID,
            const std::string& a_idPrefix)
        {
            struct NamedBand
            {
                std::string_view key;
                double WeatherPatcher::HueShiftBands::* value;
            };
            static constexpr std::array bands{
                NamedBand{ "red", &WeatherPatcher::HueShiftBands::red },
                NamedBand{ "orange", &WeatherPatcher::HueShiftBands::orange },
                NamedBand{ "yellow", &WeatherPatcher::HueShiftBands::yellow },
                NamedBand{ "green", &WeatherPatcher::HueShiftBands::green },
                NamedBand{ "teal", &WeatherPatcher::HueShiftBands::teal },
                NamedBand{ "blue", &WeatherPatcher::HueShiftBands::blue },
                NamedBand{ "magenta", &WeatherPatcher::HueShiftBands::magenta },
            };

            auto profile = a_profile;
            auto& adjustments = TuningUtil::GetSettings(profile).filteredWeatherAdjustments;
            std::vector<WeatherPatcher::HueShiftBands> direct(a_fields.size());
            for (std::size_t fieldIndex = 0; fieldIndex < a_fields.size(); ++fieldIndex)
            {
                for (const auto& band : bands)
                {
                    const auto id = std::string(a_controlID) + "_" + Lowercase(std::string(a_fields[fieldIndex].key)) +
                                    "_" + std::string(band.key);
                    const auto* rule = TuningUtil::FindFilteredWeatherRule(a_profile, id);
                    if (!rule)
                    {
                        return false;
                    }
                    auto [entry, inserted] = adjustments.try_emplace(id, rule->defaultValue);
                    (void) inserted;
                    direct[fieldIndex].*band.value = entry->second;
                }
            }

            const auto changed = DrawHueShiftCategory(a_fields, a_idPrefix, std::addressof(direct));
            if (changed)
            {
                for (std::size_t fieldIndex = 0; fieldIndex < a_fields.size(); ++fieldIndex)
                {
                    for (const auto& band : bands)
                    {
                        const auto id = std::string(a_controlID) + "_" + Lowercase(std::string(a_fields[fieldIndex].key)) +
                                        "_" + std::string(band.key);
                        adjustments[id] = direct[fieldIndex].*band.value;
                    }
                }
            }
            return changed;
        }

        std::vector<NamedLinkable> WeatherBrightnessFields(
            WeatherPatcher::BrightnessSettings& a_settings,
            WeatherPatcher::WeatherLinks& a_links)
        {
            return {
                { "sunlight", "Sunlight", &a_links.sunlight, &a_settings.sunlight },
                { "ambient", "Ambient", &a_links.ambient, &a_settings.ambient },
                { "effectLighting", "Effect Lighting", &a_links.effectLighting, &a_settings.effectLighting },
                { "fogFar", "Fog Far", &a_links.fogFar, &a_settings.fogFar },
                { "fogNear", "Fog Near", &a_links.fogNear, &a_settings.fogNear },
                { "water", "Water", &a_links.water, &a_settings.water },
                { "skyStatics", "Sky Statics", &a_links.skyStatics, &a_settings.skyStatics },
                { "skyUpper", "Sky Upper", &a_links.skyUpper, &a_settings.skyUpper },
                { "skyLower", "Sky Lower", &a_links.skyLower, &a_settings.skyLower },
                { "horizon", "Horizon", &a_links.horizon, &a_settings.horizon },
                { "sun", "Sun", &a_links.sun, &a_settings.sun },
                { "sunGlare", "Sun Glare", &a_links.sunGlare, &a_settings.sunGlare },
                { "moonGlare", "Moon Glare", &a_links.moonGlare, &a_settings.moonGlare },
                { "stars", "Stars", &a_links.stars, &a_settings.stars },
                { "cloudLayers", "Cloud Layers", &a_links.cloudLayers, &a_settings.cloudLayers },
            };
        }

        std::vector<NamedLinkable> WeatherSaturationFields(
            WeatherPatcher::SaturationSettings& a_settings,
            WeatherPatcher::WeatherLinks& a_links)
        {
            return {
                { "sunlight", "Sunlight", &a_links.sunlight, &a_settings.sunlight },
                { "ambient", "Ambient", &a_links.ambient, &a_settings.ambient },
                { "effectLighting", "Effect Lighting", &a_links.effectLighting, &a_settings.effectLighting },
                { "fogFar", "Fog Far", &a_links.fogFar, &a_settings.fogFar },
                { "fogNear", "Fog Near", &a_links.fogNear, &a_settings.fogNear },
                { "water", "Water", &a_links.water, &a_settings.water },
                { "skyStatics", "Sky Statics", &a_links.skyStatics, &a_settings.skyStatics },
                { "skyUpper", "Sky Upper", &a_links.skyUpper, &a_settings.skyUpper },
                { "skyLower", "Sky Lower", &a_links.skyLower, &a_settings.skyLower },
                { "horizon", "Horizon", &a_links.horizon, &a_settings.horizon },
                { "sun", "Sun", &a_links.sun, &a_settings.sun },
                { "sunGlare", "Sun Glare", &a_links.sunGlare, &a_settings.sunGlare },
                { "moonGlare", "Moon Glare", &a_links.moonGlare, &a_settings.moonGlare },
                { "stars", "Stars", &a_links.stars, &a_settings.stars },
                { "cloudLayers", "Cloud Layers", &a_links.cloudLayers, &a_settings.cloudLayers },
                { "volumetricLighting", "Volumetric Lighting", &a_links.volumetricLighting, &a_settings.volumetricLighting },
            };
        }

        template <class Settings>
        std::vector<NamedLinkable> WeatherCompressionFields(
            Settings& a_settings,
            WeatherPatcher::WeatherLinks& a_links)
        {
            return {
                { "sunlight", "Sunlight", &a_links.sunlight, &a_settings.sunlight },
                { "ambient", "Ambient", &a_links.ambient, &a_settings.ambient },
                { "effectLighting", "Effect Lighting", &a_links.effectLighting, &a_settings.effectLighting },
                { "fogFar", "Fog Far", &a_links.fogFar, &a_settings.fogFar },
                { "fogNear", "Fog Near", &a_links.fogNear, &a_settings.fogNear },
                { "water", "Water", &a_links.water, &a_settings.water },
                { "skyStatics", "Sky Statics", &a_links.skyStatics, &a_settings.skyStatics },
                { "skyUpper", "Sky Upper", &a_links.skyUpper, &a_settings.skyUpper },
                { "skyLower", "Sky Lower", &a_links.skyLower, &a_settings.skyLower },
                { "horizon", "Horizon", &a_links.horizon, &a_settings.horizon },
                { "sun", "Sun", &a_links.sun, &a_settings.sun },
                { "sunGlare", "Sun Glare", &a_links.sunGlare, &a_settings.sunGlare },
                { "moonGlare", "Moon Glare", &a_links.moonGlare, &a_settings.moonGlare },
                { "stars", "Stars", &a_links.stars, &a_settings.stars },
            };
        }

        std::vector<NamedValue> HueScaleFields(WeatherPatcher::AmbientHueScales& a_settings)
        {
            return {
                { "red", "Red", &a_settings.red },
                { "orange", "Orange", &a_settings.orange },
                { "yellow", "Yellow", &a_settings.yellow },
                { "green", "Green", &a_settings.green },
                { "teal", "Teal", &a_settings.teal },
                { "blue", "Blue", &a_settings.blue },
                { "magenta", "Magenta", &a_settings.magenta },
            };
        }

        std::vector<NamedValue> HueShiftBandFields(WeatherPatcher::HueShiftBands& a_settings)
        {
            return {
                { "red", "Red", &a_settings.red },
                { "orange", "Orange", &a_settings.orange },
                { "yellow", "Yellow", &a_settings.yellow },
                { "green", "Green", &a_settings.green },
                { "teal", "Teal", &a_settings.teal },
                { "blue", "Blue", &a_settings.blue },
                { "magenta", "Magenta", &a_settings.magenta },
            };
        }

        std::vector<NamedHueShift> HueShiftFields(
            WeatherPatcher::HueShiftSettings& a_settings,
            WeatherPatcher::WeatherLinks& a_links)
        {
            return {
                { "sunlight", "Sunlight", &a_links.sunlight, &a_settings.sunlight },
                { "ambient", "Ambient", &a_links.ambient, &a_settings.ambient },
                { "effectLighting", "Effect Lighting", &a_links.effectLighting, &a_settings.effectLighting },
                { "fogFar", "Fog Far", &a_links.fogFar, &a_settings.fogFar },
                { "fogNear", "Fog Near", &a_links.fogNear, &a_settings.fogNear },
                { "water", "Water", &a_links.water, &a_settings.water },
                { "skyStatics", "Sky Statics", &a_links.skyStatics, &a_settings.skyStatics },
                { "skyUpper", "Sky Upper", &a_links.skyUpper, &a_settings.skyUpper },
                { "skyLower", "Sky Lower", &a_links.skyLower, &a_settings.skyLower },
                { "horizon", "Horizon", &a_links.horizon, &a_settings.horizon },
                { "sun", "Sun", &a_links.sun, &a_settings.sun },
                { "sunGlare", "Sun Glare", &a_links.sunGlare, &a_settings.sunGlare },
                { "moonGlare", "Moon Glare", &a_links.moonGlare, &a_settings.moonGlare },
                { "stars", "Stars", &a_links.stars, &a_settings.stars },
                { "cloudLayers", "Cloud Layers", &a_links.cloudLayers, &a_settings.cloudLayers },
                { "volumetricLighting", "Volumetric Lighting", &a_links.volumetricLighting, &a_settings.volumetricLighting },
            };
        }

        std::vector<NamedHueShift> InteriorHueShiftFields(
            LightingPatcher::InteriorHueShiftSettings& a_settings,
            LightingPatcher::InteriorLinks& a_links)
        {
            return {
                { "ambientColors", "Ambient Colors (DALC)", &a_links.ambientColors, &a_settings.ambientColors },
                { "ambient", "Ambient", &a_links.ambient, &a_settings.ambient },
                { "directional", "Directional", &a_links.directional, &a_settings.directional },
                { "fogFar", "Fog Far", &a_links.fogFar, &a_settings.fogFar },
                { "fogNear", "Fog Near", &a_links.fogNear, &a_settings.fogNear },
            };
        }

        std::vector<NamedLinkable> InteriorColorFields(
            LightingPatcher::InteriorColorSettings& a_settings,
            LightingPatcher::InteriorLinks& a_links)
        {
            return {
                { "ambientColors", "Ambient Colors (DALC)", &a_links.ambientColors, &a_settings.ambientColors },
                { "ambient", "Ambient", &a_links.ambient, &a_settings.ambient },
                { "directional", "Directional", &a_links.directional, &a_settings.directional },
                { "fogFar", "Fog Far", &a_links.fogFar, &a_settings.fogFar },
                { "fogNear", "Fog Near", &a_links.fogNear, &a_settings.fogNear },
            };
        }

        bool DrawLinkOnlyCategory(
            const std::string_view a_heading,
            const std::span<const NamedLinkable> a_fields,
            const std::string& a_idPrefix,
            const double a_neutral,
            const float a_scaleMaximum = 4.0f)
        {
            if (!a_heading.empty()) DrawHeader(a_heading);
            return DrawLinkableCategory(
                a_fields,
                0.0f,
                1.0f,
                a_idPrefix,
                0.0f,
                a_neutral,
                a_scaleMaximum,
                true);
        }

        std::vector<NamedLinkable> WeatherLinkFields(WeatherPatcher::WeatherLinks& a_links)
        {
            WeatherPatcher::BrightnessSettings direct;
            auto fields = WeatherBrightnessFields(direct, a_links);
            fields.push_back({ "volumetricLighting", "Volumetric Lighting", &a_links.volumetricLighting });
            for (auto& field : fields) field.direct = nullptr;
            return fields;
        }

        std::vector<NamedLinkable> InteriorLinkFields(LightingPatcher::InteriorLinks& a_links)
        {
            LightingPatcher::InteriorColorSettings direct;
            auto fields = InteriorColorFields(direct, a_links);
            for (auto& field : fields) field.direct = nullptr;
            return fields;
        }

        bool DrawWeatherLinks(
            TuningUtil::Settings& a_settings,
            const std::string& a_prefix,
            const std::string_view a_heading)
        {
            return DrawLinkOnlyCategory(
                a_heading,
                WeatherLinkFields(a_settings.links.weather),
                a_prefix,
                0.0,
                6.0f);
        }

        bool DrawInteriorLinks(
            TuningUtil::Settings& a_settings,
            const std::string& a_prefix,
            const std::string_view a_heading)
        {
            return DrawLinkOnlyCategory(
                a_heading,
                InteriorLinkFields(a_settings.links.interior),
                a_prefix,
                0.0,
                6.0f);
        }

        bool DrawGlobalWeatherFilters(
            const MenuDefinition& a_menu,
            TuningUtil::Settings& a_settings,
            const std::string_view a_id)
        {
            const auto originalInclusions = a_settings.weatherInclusions;
            const auto originalExclusions = a_settings.weatherExclusions;
            const auto id = std::string(a_id);
            auto& state = weatherFilterEditorStates[id];
            const auto& weatherEntries = GetSliderCreatorWeatherEntries(a_menu.profile);
            auto selected = std::ranges::find(weatherEntries, state.selectedWeather, &WeatherMenuEntry::weather);
            if (selected == weatherEntries.end()) state.selectedWeather = nullptr;

            const auto weatherPreview = state.selectedWeather ?
                                            WeatherDisplayLabel(state.selectedWeather) :
                                            DisplayText("selectWeather");
            const auto weatherLabel = SKSEMenuSettings::Label("weatherFilterWeather", "Weather") +
                                      "##WeatherFilter" + id;
            if (ImGuiMCP::BeginCombo(
                    weatherLabel.c_str(),
                    weatherPreview.c_str(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                for (const auto& entry : weatherEntries)
                {
                    const auto label = entry.label + "##WeatherFilter" + id +
                                       std::format("{:08X}", entry.weather->GetFormID());
                    if (ImGuiMCP::Selectable(label.c_str(), entry.weather == state.selectedWeather))
                        state.selectedWeather = entry.weather;
                }
                ImGuiMCP::EndCombo();
            }

            const auto weatherKey = WeatherExclusionKey(state.selectedWeather);
            ImGuiMCP::BeginDisabled(weatherKey.empty());
            if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToIncluded", "Add to Included") +
                                  "##WeatherFilter" + id)
                        .c_str()))
            {
                AddUniqueString(a_settings.weatherInclusions.formIDs, weatherKey);
                std::erase_if(a_settings.weatherExclusions.formIDs, [&](const auto& a_value)
                    { return Config::IEquals(a_value, weatherKey); });
            }
            SameActionLine();
            if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToExcluded", "Add to Excluded") +
                                  "##WeatherFilter" + id)
                        .c_str()))
            {
                AddUniqueString(a_settings.weatherExclusions.formIDs, weatherKey);
                std::erase_if(a_settings.weatherInclusions.formIDs, [&](const auto& a_value)
                    { return Config::IEquals(a_value, weatherKey); });
            }
            ImGuiMCP::EndDisabled();

            DrawHeader(SKSEMenuSettings::Label("includedWeathers", "Included Weathers"));
            DrawCreatorWeatherList(
                a_settings.weatherInclusions.formIDs,
                SKSEMenuSettings::Label("includedList", "Included List") + "##" + id);
            DrawCreatorContainsList(
                a_settings.weatherInclusions.contains,
                state.includeContainsInput,
                state.includeContainsSelection,
                "WeatherFilterInclude" + id);

            DrawHeader(SKSEMenuSettings::Label("excludedWeathers", "Excluded Weathers"));
            DrawCreatorWeatherList(
                a_settings.weatherExclusions.formIDs,
                SKSEMenuSettings::Label("excludedList", "Excluded List") + "##" + id);
            DrawCreatorContainsList(
                a_settings.weatherExclusions.contains,
                state.excludeContainsInput,
                state.excludeContainsSelection,
                "WeatherFilterExclude" + id);

            const auto changed = originalInclusions != a_settings.weatherInclusions ||
                                 originalExclusions != a_settings.weatherExclusions;
            if (changed) weatherMenuEntries.erase(a_menu.profile);
            return changed;
        }

        bool DrawGlobalPluginFilters(
            const MenuDefinition& a_menu,
            TuningUtil::Settings& a_settings,
            const std::string_view a_id)
        {
            const auto originalInclusions = a_settings.pluginInclusions;
            const auto originalExclusions = a_settings.pluginExclusions;
            const auto id = std::string(a_id);
            auto& state = pluginFilterEditorStates[id];

            DrawHeader(SKSEMenuSettings::Label("includedPlugins", "Included Plugins"));
            DrawCreatorPluginList(
                a_settings.pluginInclusions.exact,
                state.includeExact.input,
                state.includeExact.selection,
                "GlobalPluginIncludeExact" + id);
            DrawCreatorPluginContainsList(
                a_settings.pluginInclusions.contains,
                state.includeContains.input,
                state.includeContains.selection,
                "GlobalPluginIncludeContains" + id);

            DrawHeader(SKSEMenuSettings::Label("excludedPlugins", "Excluded Plugins"));
            DrawCreatorPluginList(
                a_settings.pluginExclusions.exact,
                state.excludeExact.input,
                state.excludeExact.selection,
                "GlobalPluginExcludeExact" + id);
            DrawCreatorPluginContainsList(
                a_settings.pluginExclusions.contains,
                state.excludeContains.input,
                state.excludeContains.selection,
                "GlobalPluginExcludeContains" + id);

            const auto changed = originalInclusions != a_settings.pluginInclusions ||
                                 originalExclusions != a_settings.pluginExclusions;
            if (changed)
            {
                weatherMenuEntries.erase(a_menu.profile);
                sliderCreatorWeatherEntries.erase(a_menu.profile);
            }
            return changed;
        }

        bool DrawRecordFilterEditor(
            std::vector<std::string>& a_includedForms,
            std::vector<std::string>& a_excludedForms,
            std::vector<std::string>* a_includedContains,
            std::vector<std::string>* a_excludedContains,
            TuningUtil::PluginFilter& a_pluginInclusions,
            TuningUtil::PluginFilter& a_pluginExclusions,
            const std::span<const RecordMenuEntry> a_entries,
            const RecordFilterKind a_kind,
            const std::string_view a_selectorLabel,
            const std::string_view a_id)
        {
            const auto originalIncludedForms = a_includedForms;
            const auto originalExcludedForms = a_excludedForms;
            const auto originalIncludedContains =
                a_includedContains ? *a_includedContains : std::vector<std::string>{};
            const auto originalExcludedContains =
                a_excludedContains ? *a_excludedContains : std::vector<std::string>{};
            const auto originalPluginInclusions = a_pluginInclusions;
            const auto originalPluginExclusions = a_pluginExclusions;
            const auto id = std::string(a_id);
            auto& state = recordFilterEditorStates[id];
            const auto selected = std::ranges::find(a_entries, state.selectedRecord, &RecordMenuEntry::form);
            if (selected == a_entries.end()) state.selectedRecord = nullptr;

            const auto recordPreview = state.selectedRecord ?
                                           RecordFilter::DisplayName(state.selectedRecord) :
                                           DisplayText("selectRecord");
            const auto recordLabel = std::string(a_selectorLabel) + "##RecordFilter" + id;
            if (ImGuiMCP::BeginCombo(
                    recordLabel.c_str(),
                    recordPreview.c_str(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                for (const auto& entry : a_entries)
                {
                    const auto label = entry.label + "##RecordFilter" + id +
                                       std::format("{:08X}", entry.form->GetFormID());
                    if (ImGuiMCP::Selectable(label.c_str(), entry.form == state.selectedRecord))
                    {
                        state.selectedRecord = entry.form;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            const auto recordKey = RecordFilter::FormKey(state.selectedRecord);
            ImGuiMCP::BeginDisabled(recordKey.empty());
            if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToIncluded", "Add to Included") +
                                  "##RecordFilter" + id)
                        .c_str()))
            {
                AddUniqueString(a_includedForms, recordKey);
                std::erase_if(a_excludedForms, [&](const auto& a_value)
                    { return Config::IEquals(a_value, recordKey); });
            }
            SameActionLine();
            if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToExcluded", "Add to Excluded") +
                                  "##RecordFilter" + id)
                        .c_str()))
            {
                AddUniqueString(a_excludedForms, recordKey);
                std::erase_if(a_includedForms, [&](const auto& a_value)
                    { return Config::IEquals(a_value, recordKey); });
            }
            ImGuiMCP::EndDisabled();

            DrawHeader(SKSEMenuSettings::Label("includedRecords", "Included Records"));
            DrawCreatorRecordList(
                a_includedForms,
                SKSEMenuSettings::Label("includedList", "Included List") + "##" + id,
                a_kind);
            if (a_includedContains)
            {
                DrawCreatorContainsList(
                    *a_includedContains,
                    state.includeContainsInput,
                    state.includeContainsSelection,
                    "RecordFilterInclude" + id);
            }

            DrawHeader(SKSEMenuSettings::Label("excludedRecords", "Excluded Records"));
            DrawCreatorRecordList(
                a_excludedForms,
                SKSEMenuSettings::Label("excludedList", "Excluded List") + "##" + id,
                a_kind);
            if (a_excludedContains)
            {
                DrawCreatorContainsList(
                    *a_excludedContains,
                    state.excludeContainsInput,
                    state.excludeContainsSelection,
                    "RecordFilterExclude" + id);
            }

            DrawHeader(SKSEMenuSettings::Label("includedPlugins", "Included Plugins"));
            DrawCreatorPluginList(
                a_pluginInclusions.exact,
                state.plugins.includeExact.input,
                state.plugins.includeExact.selection,
                "RecordPluginIncludeExact" + id);
            DrawCreatorPluginContainsList(
                a_pluginInclusions.contains,
                state.plugins.includeContains.input,
                state.plugins.includeContains.selection,
                "RecordPluginIncludeContains" + id);

            DrawHeader(SKSEMenuSettings::Label("excludedPlugins", "Excluded Plugins"));
            DrawCreatorPluginList(
                a_pluginExclusions.exact,
                state.plugins.excludeExact.input,
                state.plugins.excludeExact.selection,
                "RecordPluginExcludeExact" + id);
            DrawCreatorPluginContainsList(
                a_pluginExclusions.contains,
                state.plugins.excludeContains.input,
                state.plugins.excludeContains.selection,
                "RecordPluginExcludeContains" + id);

            return originalIncludedForms != a_includedForms ||
                   originalExcludedForms != a_excludedForms ||
                   (a_includedContains &&
                    originalIncludedContains != *a_includedContains) ||
                   (a_excludedContains &&
                    originalExcludedContains != *a_excludedContains) ||
                   originalPluginInclusions != a_pluginInclusions ||
                   originalPluginExclusions != a_pluginExclusions;
        }

        bool DrawRecordFilterEditor(
            TuningUtil::WeatherFilter& a_inclusions,
            TuningUtil::WeatherFilter& a_exclusions,
            TuningUtil::PluginFilter& a_pluginInclusions,
            TuningUtil::PluginFilter& a_pluginExclusions,
            const std::span<const RecordMenuEntry> a_entries,
            const RecordFilterKind a_kind,
            const std::string_view a_selectorLabel,
            const std::string_view a_id)
        {
            return DrawRecordFilterEditor(
                a_inclusions.formIDs,
                a_exclusions.formIDs,
                std::addressof(a_inclusions.contains),
                std::addressof(a_exclusions.contains),
                a_pluginInclusions,
                a_pluginExclusions,
                a_entries,
                a_kind,
                a_selectorLabel,
                a_id);
        }

        bool DrawRecordFilterEditor(
            std::vector<std::string>& a_inclusions,
            std::vector<std::string>& a_exclusions,
            TuningUtil::PluginFilter& a_pluginInclusions,
            TuningUtil::PluginFilter& a_pluginExclusions,
            const std::span<const RecordMenuEntry> a_entries,
            const RecordFilterKind a_kind,
            const std::string_view a_selectorLabel,
            const std::string_view a_id)
        {
            return DrawRecordFilterEditor(
                a_inclusions,
                a_exclusions,
                nullptr,
                nullptr,
                a_pluginInclusions,
                a_pluginExclusions,
                a_entries,
                a_kind,
                a_selectorLabel,
                a_id);
        }

        void DrawSetupModule(
            const MenuDefinition& a_menu,
            const bool a_weather,
            const std::size_t a_index)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto changed = false;
            const auto moduleID = profile + (a_weather ? "WeatherSetup" : "InteriorSetup") +
                                  std::to_string(a_index);
            const auto drawBox = [&](const std::string_view a_id,
                                     const std::string_view a_title,
                                     auto&& a_draw)
            {
                constexpr auto flags = ImGuiMCP::ImGuiChildFlags_Border |
                                       ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                                       ImGuiMCP::ImGuiChildFlags_AutoResizeY;
                const auto padding = SKSEMenuSettings::GetBoxPadding();
                const auto customPadding = padding[0] > 0.0f || padding[1] > 0.0f;
                if (customPadding)
                {
                    ImGuiMCP::PushStyleVar(
                        ImGuiMCP::ImGuiStyleVar_WindowPadding,
                        ImGuiMCP::ImVec2(padding[0], padding[1]));
                }
                const auto visible = ImGuiMCP::BeginChild(
                    (moduleID + std::string(a_id) + "Box").c_str(),
                    ImGuiMCP::ImVec2(0.0f, 0.0f),
                    flags);
                auto boxChanged = false;
                if (visible)
                {
                    DrawHeader(a_title);
                    boxChanged = a_draw();
                }
                ImGuiMCP::EndChild();
                if (customPadding) ImGuiMCP::PopStyleVar();
                ImGuiMCP::Spacing();
                return boxChanged;
            };

            if (a_weather)
            {
                changed |= drawBox(
                    "CompressionAnchor",
                    SKSEMenuSettings::Label("compressionAnchor", "Compression Anchor"),
                    [&]
                    {
                        const std::array fields{
                            NamedValue{ "ambient", "Ambient Anchor", &settings.compressionAnchor.ambient },
                        };
                        return DrawValueOnlyCategory(fields, 0.0f, 255.0f, moduleID + "CompressionAnchor", 1.0f);
                    });
                changed |= drawBox(
                    "SaturationScales",
                    SKSEMenuSettings::Label("saturationScales", "Saturation Scales"),
                    [&]
                    {
                        auto hueScales = HueScaleFields(settings.hueScales);
                        return DrawValueOnlyCategory(
                            hueScales,
                            0.0f,
                            4.0f,
                            moduleID + "HueScales",
                            0.1f,
                            "hueScales");
                    });
                changed |= drawBox(
                    "HueRanges",
                    SKSEMenuSettings::Label("hueRanges", "Hue Ranges"),
                    [&]
                    {
                        return DrawHueRanges(
                            settings.hueRanges,
                            moduleID + "HueRanges");
                    });
                changed |= drawBox(
                    "WeatherFilter",
                    SKSEMenuSettings::Label("weatherFilter", "Weather Filter"),
                    [&]
                    { return DrawGlobalWeatherFilters(a_menu, settings, moduleID + "WeatherFilter"); });
                changed |= drawBox(
                    "PluginFilter",
                    SKSEMenuSettings::Label("pluginFilter", "Plugin Filter"),
                    [&]
                    { return DrawGlobalPluginFilters(a_menu, settings, moduleID + "PluginFilter"); });
            }
            else
            {
                changed |= drawBox(
                    "InteriorAmbientLinks",
                    SKSEMenuSettings::Label("interiorAmbientLinks", "Interior Ambient Links"),
                    [&]
                    {
                        auto fields = InteriorLinkFields(settings.links.interior);
                        return DrawLinkOnlyCategory(
                            {},
                            fields,
                            moduleID + "InteriorLinks",
                            0.0,
                            6.0f);
                    });
                changed |= drawBox(
                    "InteriorSaturationScales",
                    SKSEMenuSettings::Label("interiorSaturationScales", "Interior Saturation Scales"),
                    [&]
                    {
                        auto hueScales = HueScaleFields(settings.intAmbientHueScales);
                        return DrawValueOnlyCategory(
                            hueScales,
                            0.0f,
                            4.0f,
                            moduleID + "InteriorHueScales",
                            0.1f,
                            "intAmbientHueScales");
                    });
                const auto pointLightHueScalesChanged = drawBox(
                    "LightingBulbSaturationScales",
                    SKSEMenuSettings::Label(
                        "lightingBulbSaturationScales",
                        "Lighting Bulb Saturation Scales"),
                    [&]
                    {
                        auto hueScales = HueScaleFields(settings.pointLights.hueScales);
                        return DrawValueOnlyCategory(
                            hueScales,
                            0.0f,
                            4.0f,
                            moduleID + "LightingBulbHueScales",
                            0.1f,
                            "pointLights.hueScales");
                    });
                changed |= pointLightHueScalesChanged;
                const auto hueRangesChanged = drawBox(
                    "InteriorLightingHueRanges",
                    SKSEMenuSettings::Label(
                        "interiorLightingHueRanges",
                        "Interior & Lighting Bulb Hue Ranges"),
                    [&]
                    {
                        return DrawHueRanges(
                            settings.intHueRanges,
                            moduleID + "InteriorLightingHueRanges");
                    });
                changed |= hueRangesChanged;
                changed |= drawBox(
                    "LightingTemplateFilter",
                    SKSEMenuSettings::Label("lightingTemplateFilter", "Lighting Template Filter"),
                    [&]
                    {
                        return DrawRecordFilterEditor(
                            settings.lightingTemplateInclusions,
                            settings.lightingTemplateExclusions,
                            settings.lightingTemplatePluginInclusions,
                            settings.lightingTemplatePluginExclusions,
                            GetLightingTemplateMenuEntries(),
                            RecordFilterKind::lightingTemplate,
                            SKSEMenuSettings::Label("lightingTemplate", "Lighting Template"),
                            moduleID + "LightingTemplateFilter");
                    });
                changed |= drawBox(
                    "EffectLightingFilter",
                    SKSEMenuSettings::Label("effectLightingFilter", "Effect Lighting Filter"),
                    [&]
                    {
                        return DrawRecordFilterEditor(
                            settings.effectPointLightInclusions,
                            settings.effectPointLightExclusions,
                            settings.effectLightingPluginInclusions,
                            settings.effectLightingPluginExclusions,
                            GetEffectLightingMenuEntries(),
                            RecordFilterKind::effectLighting,
                            SKSEMenuSettings::Label("fxWeather", "FX Weather"),
                            moduleID + "EffectLightingFilter");
                    });
                if (changed)
                {
                    ApplySliderChange(pointLightHueScalesChanged || hueRangesChanged);
                }
                return;
            }

            if (changed) ApplySliderChange(false);
        }

        void DrawLinksModule(const MenuDefinition& a_menu, const MenuControl& a_control)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto changed = false;
            if (Config::IEquals(a_control.setting, "weather"))
                changed |= DrawWeatherLinks(
                    settings,
                    profile + "LinksWeather",
                    ControlDisplayName(a_control, "Weather Links"));
            else if (Config::IEquals(a_control.setting, "interior"))
                changed |= DrawInteriorLinks(
                    settings,
                    profile + "LinksInterior",
                    ControlDisplayName(a_control, "Interior Links"));

            if (changed) ApplySliderChange(false);
        }

        std::optional<SliderSetting> FindLinkableSlider(
            const std::span<const NamedLinkable> a_fields,
            const std::string_view a_setting,
            const std::string_view a_prefix,
            const double a_linkNeutral)
        {
            if (!a_setting.starts_with(a_prefix)) return std::nullopt;
            const auto field = std::ranges::find(a_fields, a_setting.substr(a_prefix.size()), &NamedLinkable::key);
            if (field == a_fields.end()) return std::nullopt;

            const auto values = ResolveLinkableValues(a_fields, a_linkNeutral);
            return SliderSetting{
                .resolved = values[static_cast<std::size_t>(std::distance(a_fields.begin(), field))],
                .link = field->value,
                .scalar = field->direct,
            };
        }

        std::optional<SliderSetting> FindValueSlider(
            const std::span<const NamedValue> a_fields,
            const std::string_view a_setting,
            const std::string_view a_prefix)
        {
            if (!a_setting.starts_with(a_prefix)) return std::nullopt;
            const auto field = std::ranges::find(a_fields, a_setting.substr(a_prefix.size()), &NamedValue::key);
            return field == a_fields.end() ?
                       std::nullopt :
                       std::optional{ SliderSetting{ .resolved = *field->value, .scalar = field->value } };
        }

        std::optional<SliderSetting> FindHueShiftSlider(
            const std::span<const NamedHueShift> a_fields,
            const std::string_view a_setting,
            const std::string_view a_prefix)
        {
            if (!a_setting.starts_with(a_prefix)) return std::nullopt;
            const auto path = a_setting.substr(a_prefix.size());
            const auto separator = path.find('.');
            if (separator == std::string_view::npos || path.find('.', separator + 1) != std::string_view::npos)
                return std::nullopt;

            const auto field = std::ranges::find(a_fields, path.substr(0, separator), &NamedHueShift::key);
            if (field == a_fields.end()) return std::nullopt;

            const auto bandName = path.substr(separator + 1);
            double WeatherPatcher::HueShiftBands::* band = nullptr;
            if (bandName == "red") band = &WeatherPatcher::HueShiftBands::red;
            else if (bandName == "orange") band = &WeatherPatcher::HueShiftBands::orange;
            else if (bandName == "yellow") band = &WeatherPatcher::HueShiftBands::yellow;
            else if (bandName == "green") band = &WeatherPatcher::HueShiftBands::green;
            else if (bandName == "teal") band = &WeatherPatcher::HueShiftBands::teal;
            else if (bandName == "blue") band = &WeatherPatcher::HueShiftBands::blue;
            else if (bandName == "magenta") band = &WeatherPatcher::HueShiftBands::magenta;
            else return std::nullopt;

            const auto resolved = ResolveHueShiftValues(a_fields);
            const auto value = resolved[static_cast<std::size_t>(std::distance(a_fields.begin(), field))];
            return SliderSetting{
                .resolved = value.*band,
                .link = field->value,
                .hueShift = field->direct,
                .resolvedHueShift = value,
                .hueBand = band,
            };
        }

        std::vector<NamedValue> HueRangeFields(WeatherPatcher::HueRanges& a_ranges)
        {
            return {
                { "red.start", {}, &a_ranges.red.start },
                { "red.end", {}, &a_ranges.red.end },
                { "orange.start", {}, &a_ranges.orange.start },
                { "orange.end", {}, &a_ranges.orange.end },
                { "yellow.start", {}, &a_ranges.yellow.start },
                { "yellow.end", {}, &a_ranges.yellow.end },
                { "green.start", {}, &a_ranges.green.start },
                { "green.end", {}, &a_ranges.green.end },
                { "teal.start", {}, &a_ranges.teal.start },
                { "teal.end", {}, &a_ranges.teal.end },
                { "blue.start", {}, &a_ranges.blue.start },
                { "blue.end", {}, &a_ranges.blue.end },
                { "magenta.start", {}, &a_ranges.magenta.start },
                { "magenta.end", {}, &a_ranges.magenta.end },
            };
        }

        std::vector<NamedValue> ImageSpaceFields(WeatherPatcher::ImageSpaceSettings& a_settings)
        {
            return {
                { "saturationMultiplier", {}, &a_settings.saturationMultiplier },
                { "brightnessMultiplier", {}, &a_settings.brightnessMultiplier },
                { "contrastMultiplier", {}, &a_settings.contrastMultiplier },
                { "sunlightScaleMultiplier", {}, &a_settings.sunlightScaleMultiplier },
                { "skyScaleMultiplier", {}, &a_settings.skyScaleMultiplier },
            };
        }

        std::optional<SliderSetting> FindSliderSetting(
            TuningUtil::Settings& a_settings,
            const std::string_view a_setting)
        {
            if (!SliderSettingCatalog::Find(a_setting)) return std::nullopt;
            const auto linkable = [&](auto fields, const std::string_view a_prefix, const double a_neutral = 0.0)
            {
                return FindLinkableSlider(fields, a_setting, a_prefix, a_neutral);
            };
            auto& weatherLinks = a_settings.links.weather;
            auto& interiorLinks = a_settings.links.interior;
            if (auto value = linkable(WeatherBrightnessFields(a_settings.brightnessMultiplier, weatherLinks), "brightnessMultiplier.", 1.0)) return value;
            if (auto value = linkable(WeatherSaturationFields(a_settings.saturationMultiplier, weatherLinks), "saturationMultiplier.")) return value;
            if (auto value = linkable(WeatherCompressionFields(a_settings.betweenWeatherCompression, weatherLinks), "betweenWeatherCompression.")) return value;
            if (auto value = linkable(WeatherCompressionFields(a_settings.withinWeatherCompression, weatherLinks), "withinWeatherCompression.")) return value;
            if (auto value = linkable(WeatherCompressionFields(a_settings.compressionAnchor, weatherLinks), "compressionAnchor.")) return value;
            if (auto value = linkable(InteriorColorFields(a_settings.intBrightnessMultiplier, interiorLinks), "intBrightnessMultiplier.")) return value;
            if (auto value = linkable(InteriorColorFields(a_settings.intSaturationMultiplier, interiorLinks), "intSaturationMultiplier.")) return value;
            if (auto value = FindHueShiftSlider(HueShiftFields(a_settings.hueShift, weatherLinks), a_setting, "hueShift.")) return value;
            if (auto value = FindHueShiftSlider(InteriorHueShiftFields(a_settings.intHueShift, interiorLinks), a_setting, "intHueShift.")) return value;

            const auto scalar = [&](auto fields, const std::string_view a_prefix)
            {
                return FindValueSlider(fields, a_setting, a_prefix);
            };
            if (auto value = scalar(HueScaleFields(a_settings.hueScales), "hueScales.")) return value;
            if (auto value = scalar(HueScaleFields(a_settings.intAmbientHueScales), "intAmbientHueScales.")) return value;
            if (auto value = scalar(HueShiftBandFields(a_settings.fxEffectLighting.hueShift), "fxEffectLighting.hueShift.")) return value;
            if (auto value = scalar(HueScaleFields(a_settings.pointLights.hueScales), "pointLights.hueScales.")) return value;
            if (auto value = scalar(HueShiftBandFields(a_settings.pointLights.hueShift), "pointLights.hueShift.")) return value;
            if (auto value = scalar(HueRangeFields(a_settings.hueRanges), "hueRanges.")) return value;
            if (auto value = scalar(HueRangeFields(a_settings.intHueRanges), "intHueRanges.")) return value;
            if (auto value = scalar(ImageSpaceFields(a_settings.exteriorImageSpace), "exteriorImageSpace.")) return value;
            if (auto value = scalar(ImageSpaceFields(a_settings.intImageSpace), "intImageSpace.")) return value;
            if (a_setting == "volumetricLightingIntensityMultiplier")
                return SliderSetting{ .resolved = a_settings.volumetricLightingIntensityMultiplier, .scalar = &a_settings.volumetricLightingIntensityMultiplier };
            if (a_setting == "intFogMaxMultiplier")
                return SliderSetting{ .resolved = a_settings.intFogMaxMultiplier, .scalar = &a_settings.intFogMaxMultiplier };
            if (a_setting == "fxEffectLighting.brightnessMultiplier")
                return SliderSetting{ .resolved = a_settings.fxEffectLighting.brightnessMultiplier, .scalar = &a_settings.fxEffectLighting.brightnessMultiplier };
            if (a_setting == "fxEffectLighting.saturationMultiplier")
                return SliderSetting{ .resolved = a_settings.fxEffectLighting.saturationMultiplier, .scalar = &a_settings.fxEffectLighting.saturationMultiplier };
            if (a_setting == "pointLights.fadeMultiplier")
                return SliderSetting{ .resolved = a_settings.pointLights.fadeMultiplier, .scalar = &a_settings.pointLights.fadeMultiplier };
            if (a_setting == "pointLights.sunlightFadeMultiplier")
                return SliderSetting{ .resolved = a_settings.pointLights.sunlightFadeMultiplier, .scalar = &a_settings.pointLights.sunlightFadeMultiplier };
            if (a_setting == "pointLights.saturationMultiplier")
                return SliderSetting{ .resolved = a_settings.pointLights.saturationMultiplier, .scalar = &a_settings.pointLights.saturationMultiplier };
            return std::nullopt;
        }

        bool DrawGroupedSlider(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            if (profile.empty() || (a_control.settings.empty() && a_control.setting.empty()))
            {
                return false;
            }

            auto& profileSettings = TuningUtil::GetSettings(profile);
            std::vector<SliderSetting> settings;
            const auto append = [&](const std::string_view a_path, const bool a_ignoreLink)
            {
                auto setting = FindSliderSetting(profileSettings, a_path);
                if (!setting || (!a_control.link.empty() && !setting->link))
                {
                    return false;
                }
                if (a_ignoreLink && a_control.link.empty()) setting->ResolveWithoutLink();
                settings.push_back(*setting);
                return true;
            };
            settings.reserve(std::max<std::size_t>(1, a_control.settings.size()));
            if (a_control.settings.empty())
            {
                if (!append(a_control.setting, false)) return false;
            }
            else
                for (const auto& target : a_control.settings)
                {
                    if (!append(SliderTargetPath(target), SliderTargetIgnoresLink(target))) return false;
                }

            auto mixed = false;
            float value = 0.0f;
            const bool inverted = a_control.invert && a_control.link.empty();
            if (a_control.link.empty())
            {
                value = static_cast<float>(inverted ? -settings.front().resolved : settings.front().resolved);
                mixed = std::ranges::any_of(
                    settings | std::views::drop(1),
                    [&](const SliderSetting& a_setting)
                    {
                        return std::abs(a_setting.resolved - settings.front().resolved) > 0.0001;
                    });
            }
            else
            {
                const auto first = ReadLinkable(*settings.front().link);
                value = static_cast<float>(first.scale);
                mixed = !first.linked || !Config::IEquals(first.link, a_control.link) ||
                        std::ranges::any_of(
                            settings | std::views::drop(1),
                            [&](const SliderSetting& a_setting)
                            {
                                const auto parts = ReadLinkable(*a_setting.link);
                                return !parts.linked || !Config::IEquals(parts.link, a_control.link) ||
                                       std::abs(parts.scale - first.scale) > 0.0001;
                            });
            }

            const auto styleKey = a_control.settings.empty() ?
                                      std::string_view(a_control.setting) :
                                      SliderTargetPath(a_control.settings.front());
            auto sliderDefaults = ResolveControlSliderDefaults(a_control, styleKey, -1.0f);
            const auto minimum = std::min(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto maximum = std::max(sliderDefaults.minimum, sliderDefaults.maximum);
            const auto* format = IsSafeSliderFormat(sliderDefaults.format) ? sliderDefaults.format.c_str() : "%.2f";
            const auto& sliderId = a_id;
            if (DrawSliderWithInput(
                    sliderId,
                    value,
                    minimum,
                    maximum,
                    sliderDefaults.step,
                    format,
                    sliderDefaults.width,
                    SliderInputRange::standard,
                    a_control.link.empty() ? ControlNeutralValue(a_control) : std::nullopt))
            {
                for (const auto& setting : settings)
                {
                    if (a_control.link.empty())
                    {
                        setting.Set(inverted ? -value : value);
                    }
                    else
                    {
                        *setting.link = std::tuple{ a_control.link, static_cast<double>(value) };
                    }
                }
                ApplySliderChange(std::ranges::any_of(
                                      a_control.settings,
                                      [](const auto& a_target)
                                      { return AffectsLightPlacer(SliderTargetPath(a_target)); }) ||
                                  AffectsLightPlacer(a_control.setting));
            }
            if (mixed)
            {
                DrawDisplayText("mixedSliderValues", true);
            }
            return true;
        }

        bool DrawHueRanges(WeatherPatcher::HueRanges& a_ranges, const std::string& a_idPrefix)
        {
            struct NamedRange
            {
                std::string_view label;
                WeatherPatcher::HueRange* range;
            };
            const std::array ranges{
                NamedRange{ "Red", &a_ranges.red },
                NamedRange{ "Orange", &a_ranges.orange },
                NamedRange{ "Yellow", &a_ranges.yellow },
                NamedRange{ "Green", &a_ranges.green },
                NamedRange{ "Teal", &a_ranges.teal },
                NamedRange{ "Blue", &a_ranges.blue },
                NamedRange{ "Magenta", &a_ranges.magenta },
            };
            const auto normalize = [](const double a_value)
            {
                auto value = std::fmod(a_value, 255.0);
                return value < 0.0 ? value + 255.0 : value;
            };
            const auto circularDelta = [](const double a_left, const double a_right)
            {
                return std::fmod((a_right - a_left) + 382.5, 255.0) - 127.5;
            };
            const auto circularMidpoint = [&](const double a_left, const double a_right)
            {
                return normalize(a_left + circularDelta(a_left, a_right) * 0.5);
            };

            auto changed = false;
            const auto sliderStyle = SKSEMenuSettings::ResolveSliderDefaults("hueRange", 0.0f, 0.1f, "%.1f");
            const auto* sliderFormat = IsSafeSliderFormat(sliderStyle.format) ? sliderStyle.format.c_str() : "%.1f";
            for (std::size_t index = 0; index < ranges.size(); ++index)
            {
                auto& current = *ranges[index].range;
                auto& next = *ranges[(index + 1) % ranges.size()].range;
                if (std::abs(circularDelta(current.end, next.start)) <= 0.0001)
                {
                    continue;
                }
                const auto boundary = circularMidpoint(current.end, next.start);
                current.end = boundary;
                next.start = boundary;
                changed = true;
            }
            for (std::size_t index = 0; index < ranges.size(); ++index)
            {
                auto& current = *ranges[index].range;
                auto& next = *ranges[(index + 1) % ranges.size()].range;
                auto boundary = static_cast<float>(current.end);
                const auto label = std::string(ranges[index].label) + " / " + std::string(ranges[(index + 1) % ranges.size()].label);
                const auto boundaryId = label + "##" + a_idPrefix + "Boundary" + std::to_string(index);
                if (DrawSliderWithInput(
                        boundaryId,
                        boundary,
                        0.0f,
                        255.0f,
                        sliderStyle.step,
                        sliderFormat,
                        sliderStyle.width,
                        SliderInputRange::hue))
                {
                    current.end = boundary;
                    next.start = boundary;
                    changed = true;
                }
            }
            return changed;
        }

        bool ForceCSTonemappingEnabled(
            const std::string_view a_profile,
            const WeatherPatcher::ImageSpaceSettings& a_settings,
            const bool a_followAuto)
        {
            if (a_settings.ForceCSTonemapping)
            {
                return *a_settings.ForceCSTonemapping;
            }
            return a_followAuto &&
                   ImageSpacePatcher::IsAutoCSTonemappingApplied(a_profile).value_or(false);
        }

        bool ForceCSTonemappingControlAvailable(const std::string_view a_profile)
        {
            if (!Config::IEquals(a_profile, "Helios"))
            {
                return true;
            }
            ImageSpacePatcher::RequestRuntimeMonitorRefresh();
            const auto monitor = ImageSpacePatcher::ReadRuntimeMonitor();
            return monitor.filmicCurveAvailable && monitor.filmicCurve;
        }

        bool DrawImageSpaceSettings(
            const std::string_view a_profile,
            WeatherPatcher::ImageSpaceSettings& a_settings,
            const std::string& a_idPrefix,
            const std::string_view a_catalogPrefix,
            const bool a_followAuto)
        {
            auto changed = false;
            constexpr std::array<std::pair<std::string_view, std::string_view>, 5> fields{
                std::pair{ "saturationMultiplier", "Saturation" },
                std::pair{ "brightnessMultiplier", "Brightness" },
                std::pair{ "contrastMultiplier", "Contrast" },
                std::pair{ "sunlightScaleMultiplier", "Sunlight Scale" },
                std::pair{ "skyScaleMultiplier", "Sky Scale" },
            };
            const std::array<double*, 5> values{
                &a_settings.saturationMultiplier,
                &a_settings.brightnessMultiplier,
                &a_settings.contrastMultiplier,
                &a_settings.sunlightScaleMultiplier,
                &a_settings.skyScaleMultiplier,
            };
            for (std::size_t index = 0; index < fields.size(); ++index)
            {
                auto value = static_cast<float>(*values[index]);
                constexpr auto maximum = 4.0f;
                const auto id = std::string(fields[index].second) + "##" + a_idPrefix + std::string(fields[index].first);
                const auto sliderStyle = SKSEMenuSettings::ResolveSliderDefaults(
                    a_idPrefix + std::string(fields[index].first),
                    0.0f,
                    0.1f,
                    "%.1f");
                const auto* sliderFormat = IsSafeSliderFormat(sliderStyle.format) ? sliderStyle.format.c_str() : "%.1f";
                if (DrawSliderWithInput(
                        id,
                        value,
                        0.0f,
                        maximum,
                        sliderStyle.step,
                        sliderFormat,
                        sliderStyle.width,
                        SliderInputRange::standard,
                        SliderNeutralValue(
                            std::string(a_catalogPrefix) + "." +
                            std::string(fields[index].first))))
                {
                    *values[index] = value;
                    changed = true;
                }
            }
            ImGuiMCP::Separator();
            auto forceCSTonemapping = ForceCSTonemappingEnabled(
                a_profile,
                a_settings,
                a_followAuto);
            const auto controlAvailable = ForceCSTonemappingControlAvailable(a_profile);
            const auto forceLabel =
                SKSEMenuSettings::Label("forceCSTonemapping", "Force CS Tonemapping") +
                "##" + a_idPrefix;
            ImGuiMCP::BeginDisabled(!controlAvailable);
            const auto forceChanged = ImGuiMCP::Checkbox(
                    forceLabel.c_str(),
                    &forceCSTonemapping);
            ImGuiMCP::EndDisabled();
            if (forceChanged)
            {
                a_settings.ForceCSTonemapping = forceCSTonemapping;
                changed = true;
            }
            if (Config::IEquals(a_profile, "Helios"))
            {
                const auto monitor = ImageSpacePatcher::ReadRuntimeMonitor();
                const auto unavailable = SKSEMenuSettings::Label("unavailableValue", "Unavailable");
                ImGuiMCP::TextUnformatted(
                    SKSEMenuSettings::Label("displayIniSection", "[Display]").c_str());
                if (monitor.filmicCurveAvailable)
                    ImGuiMCP::Text("bUseFilmicCurve=%d", monitor.filmicCurve ? 1 : 0);
                else
                    ImGuiMCP::Text("bUseFilmicCurve=%s", unavailable.c_str());
                if (monitor.filmicWhiteScaleAvailable)
                    ImGuiMCP::Text("fFilmicWhiteScale=%.3f", monitor.filmicWhiteScale);
                else
                    ImGuiMCP::Text("fFilmicWhiteScale=%s", unavailable.c_str());
            }
            return changed;
        }

        bool DrawWeatherSettingsEditor(
            const MenuDefinition& a_menu,
            const MenuControl& a_control)
        {
            const auto a_category = std::string_view(a_control.setting);
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto changed = false;
            auto supported = true;
            const auto prefix = profile + std::string(a_category);
            if (a_category == "brightness")
            {
                auto fields = WeatherBrightnessFields(settings.brightnessMultiplier, settings.links.weather);
                if (!a_control.id.empty() && TuningUtil::FindFilteredWeatherRule(profile, a_control.id + "_ambient"))
                {
                    changed |= DrawFilteredLinkableCategory(
                        profile,
                        fields,
                        a_control.id,
                        0.1f,
                        4.0f,
                        prefix,
                        0.1f,
                        1.0,
                        4.0f,
                        "brightnessMultiplier");
                }
                else
                {
                    changed |= DrawLinkableCategory(
                        fields,
                        0.1f,
                        4.0f,
                        prefix,
                        0.1f,
                        1.0,
                        4.0f,
                        false,
                        "brightnessMultiplier");
                    ImGuiMCP::TextUnformatted("Volumetric Lighting Intensity");
                    auto intensity = static_cast<float>(settings.volumetricLightingIntensityMultiplier);
                    if (DrawConfiguredSlider(
                            "volumetricLightingIntensityMultiplier",
                            "##Value" + prefix + "volumetricIntensity",
                            intensity,
                            0.0f,
                            4.0f,
                            0.1f,
                            "%.1f"))
                    {
                        settings.volumetricLightingIntensityMultiplier = intensity;
                        changed = true;
                    }
                }
            }
            else if (a_category == "saturation")
            {
                auto fields = WeatherSaturationFields(settings.saturationMultiplier, settings.links.weather);
                if (!a_control.id.empty() && TuningUtil::FindFilteredWeatherRule(profile, a_control.id + "_ambient"))
                {
                    changed |= DrawFilteredLinkableCategory(
                        profile,
                        fields,
                        a_control.id,
                        0.0f,
                        6.0f,
                        prefix,
                        0.1f,
                        1.0,
                        6.0f,
                        "saturationMultiplier");
                }
                else
                {
                    changed |= DrawLinkableCategory(
                        fields,
                        0.0f,
                        6.0f,
                        prefix,
                        0.1f,
                        0.0,
                        6.0f,
                        false,
                        "saturationMultiplier");
                }
            }
            else if (a_category == "hueScales")
            {
                auto fields = HueScaleFields(settings.hueScales);
                changed |= DrawValueOnlyCategory(fields, 0.0f, 4.0f, prefix, 0.1f, "hueScales");
            }
            else if (a_category == "hueRanges")
            {
                changed |= DrawHueRanges(settings.hueRanges, prefix);
            }
            else if (a_category == "hueShift")
            {
                auto fields = HueShiftFields(settings.hueShift, settings.links.weather);
                if (!a_control.id.empty() && TuningUtil::FindFilteredWeatherRule(profile, a_control.id + "_ambient_red"))
                {
                    changed |= DrawFilteredHueShiftCategory(profile, fields, a_control.id, prefix);
                }
                else
                {
                    changed |= DrawHueShiftCategory(fields, prefix);
                }
            }
            else if (a_category == "betweenCompression")
            {
                auto betweenFields = WeatherCompressionFields(settings.betweenWeatherCompression, settings.links.weather);
                changed |= DrawLinkableCategory(betweenFields, -200.0f, 100.0f, prefix + "between", 10.0f);
            }
            else if (a_category == "withinCompression")
            {
                auto withinFields = WeatherCompressionFields(settings.withinWeatherCompression, settings.links.weather);
                changed |= DrawLinkableCategory(withinFields, -200.0f, 100.0f, prefix + "within", 10.0f);
            }
            else if (a_category == "exteriorImageSpace")
            {
                changed |= DrawImageSpaceSettings(
                    profile,
                    settings.exteriorImageSpace,
                    prefix,
                    "exteriorImageSpace",
                    true);
            }
            else supported = false;

            if (changed)
            {
                ApplySliderChange(AffectsLightPlacer(a_category));
            }
            return supported;
        }

        bool DrawLightingSettingsEditor(
            const MenuDefinition& a_menu,
            const std::string_view a_category)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto changed = false;
            auto supported = true;
            const auto prefix = profile + std::string(a_category);
            if (a_category == "intBrightness")
            {
                auto fields = InteriorColorFields(settings.intBrightnessMultiplier, settings.links.interior);
                changed |= DrawLinkableCategory(
                    fields,
                    0.0f,
                    10.0f,
                    prefix,
                    0.1f,
                    0.0,
                    4.0f,
                    false,
                    "intBrightnessMultiplier");
                ImGuiMCP::TextUnformatted("Fog Strength");
                auto fogMax = static_cast<float>(settings.intFogMaxMultiplier);
                if (DrawConfiguredSlider(
                        "intFogMaxMultiplier",
                        "##Value" + prefix + "fogMax",
                        fogMax,
                        0.0f,
                        4.0f,
                        0.1f,
                        "%.1f"))
                {
                    settings.intFogMaxMultiplier = fogMax;
                    changed = true;
                }
            }
            else if (a_category == "intSaturation")
            {
                auto fields = InteriorColorFields(settings.intSaturationMultiplier, settings.links.interior);
                changed |= DrawLinkableCategory(
                    fields,
                    0.0f,
                    6.0f,
                    prefix,
                    0.1f,
                    0.0,
                    6.0f,
                    false,
                    "intSaturationMultiplier");
            }
            else if (a_category == "intHueShift")
            {
                auto fields = InteriorHueShiftFields(settings.intHueShift, settings.links.interior);
                changed |= DrawHueShiftCategory(fields, prefix);
            }
            else if (a_category == "intHueScales")
            {
                auto fields = HueScaleFields(settings.intAmbientHueScales);
                changed |= DrawValueOnlyCategory(
                    fields,
                    0.0f,
                    4.0f,
                    prefix,
                    0.1f,
                    "intAmbientHueScales");
            }
            else if (a_category == "intHueRanges")
            {
                changed |= DrawHueRanges(settings.intHueRanges, prefix);
            }
            else if (a_category == "fxEffectLighting")
            {
                ImGuiMCP::TextUnformatted("Brightness");
                auto brightness = static_cast<float>(settings.fxEffectLighting.brightnessMultiplier);
                if (DrawConfiguredSlider(
                        "fxEffectLighting.brightnessMultiplier",
                        "##Value" + prefix + "brightness",
                        brightness,
                        0.1f,
                        10.0f,
                        0.1f,
                        "%.1f"))
                {
                    settings.fxEffectLighting.brightnessMultiplier = brightness;
                    changed = true;
                }

                ImGuiMCP::TextUnformatted("Saturation");
                auto saturation = static_cast<float>(settings.fxEffectLighting.saturationMultiplier);
                if (DrawConfiguredSlider(
                        "fxEffectLighting.saturationMultiplier",
                        "##Value" + prefix + "saturation",
                        saturation,
                        0.0f,
                        6.0f,
                        0.1f,
                        "%.1f"))
                {
                    settings.fxEffectLighting.saturationMultiplier = saturation;
                    changed = true;
                }

                DrawHeader("Hue Shift");
                auto hueShift = HueShiftBandFields(settings.fxEffectLighting.hueShift);
                changed |= DrawValueOnlyCategory(hueShift, -180.0f, 180.0f, prefix + "hueShift", 0.1f);
            }
            else if (a_category == "pointLights")
            {
                const auto drawMultiplier = [&](const std::string_view a_label,
                                                const std::string_view a_key,
                                                double& a_value,
                                                const float a_maximum)
                {
                    ImGuiMCP::TextUnformatted(a_label.data());
                    auto value = static_cast<float>(a_value);
                    const auto id = "##Value" + prefix + std::string(a_key);
                    if (DrawConfiguredSlider(
                            "pointLights." + std::string(a_key) + "Multiplier",
                            id,
                            value,
                            0.0f,
                            a_maximum,
                            0.1f,
                            "%.1f"))
                    {
                        a_value = value;
                        changed = true;
                    }
                    ImGuiMCP::Separator();
                };

                drawMultiplier("Brightness", "fade", settings.pointLights.fadeMultiplier, 10.0f);
                drawMultiplier(
                    "Sunlight",
                    "sunlightFade",
                    settings.pointLights.sunlightFadeMultiplier,
                    10.0f);
                drawMultiplier("Saturation", "saturation", settings.pointLights.saturationMultiplier, 6.0f);

                DrawHeader("Hue Shift");
                auto hueShift = HueShiftBandFields(settings.pointLights.hueShift);
                changed |= DrawValueOnlyCategory(hueShift, -180.0f, 180.0f, prefix + "hueShift", 0.1f);
            }
            else if (a_category == "intImageSpace")
            {
                changed |= DrawImageSpaceSettings(
                    profile,
                    settings.intImageSpace,
                    prefix,
                    "intImageSpace",
                    false);
            }
            else supported = false;

            if (changed)
            {
                ApplySliderChange(AffectsLightPlacer(a_category));
            }
            return supported;
        }

        std::string PresetVisualKey(const std::string_view a_category, const std::string_view a_preset)
        {
            auto key = Lowercase(std::string(a_category));
            key.push_back('\x1F');
            key.append(Lowercase(std::string(a_preset)));
            return key;
        }

        PendingPresetRemovals& PendingPresetRemovalState(const std::string_view a_profile)
        {
            return pendingPresetRemovals[Lowercase(std::string(a_profile))];
        }

        void RefreshAfterPresetChange(const MenuDefinition& a_menu)
        {
            weatherMenuEntries.clear();
            sliderCreatorWeatherEntries.clear();
            presetVisualStates.erase(Lowercase(a_menu.profile));
            activeWeatherLockProfile.clear();
            ActivateWeatherLockPreference(a_menu);
        }

        void DrawSavePresetSelection(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            auto visibleLabel = ControlLabel(a_control, "savePresetSelection", "Save Preset Selection");
            if (visibleLabel == "Save Preset Settings") visibleLabel = "Save Preset Selection";
            const auto label = visibleLabel + "##" + a_id;
            const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
            if (ImGuiMCP::Button(label.c_str()))
            {
                std::string error;
                const auto committed = WeatherPatcher::CommitPresetPreviews(profile, error);
                if (committed)
                {
                    RefreshAfterPresetChange(a_menu);
                    statusMessage = StatusText("presetSelectionSaved");
                }
                else
                {
                    if (!error.empty()) logger::warn("[Tuning Menu] preset selection save failed | profile={} | {}", profile, error);
                    statusMessage = StatusText("presetSelectionSaveFailure");
                }
            }
        }

        void SavePresetControl(const MenuDefinition& a_menu)
        {
            auto profile = a_menu.profile;
            auto& pending = PendingPresetRemovalState(profile);
            std::vector<std::string> categories;
            std::vector<WeatherPatcher::ActivePreset> presets;
            categories.reserve(pending.categories.size());
            presets.reserve(pending.presets.size());
            for (const auto& [key, category] : pending.categories)
            {
                (void) key;
                categories.push_back(category);
            }
            for (const auto& [key, preset] : pending.presets)
            {
                (void) key;
                presets.push_back(preset);
            }

            std::string error;
            const auto activePresets = WeatherPatcher::GetActivePresets(profile, error);
            const auto removesActivePreset = error.empty() && std::ranges::any_of(activePresets, [&](const auto& a_active)
                                                                  { return std::ranges::any_of(categories, [&](const auto& a_category)
                                                                               { return Config::IEquals(a_category, a_active.category); }) ||
                                                                           std::ranges::any_of(presets, [&](const auto& a_preset)
                                                                               { return Config::IEquals(a_preset.category, a_active.category) &&
                                                                                        Config::IEquals(a_preset.name, a_active.name); }); });
            const auto removed = error.empty() &&
                                 (categories.empty() && presets.empty() ||
                                     WeatherPatcher::RemovePresets(profile, categories, presets, error));
            const auto saved = removed && (!removesActivePreset || TuningUtil::RestoreSettings(profile));
            if (saved)
            {
                pending = {};
                auto& input = presetSaveInputs[profile];
                input.removalCategory.clear();
                input.removalPreset.clear();
                input.categoryRenameSource.clear();
                input.presetRenameSource.clear();
                input.settingsSelection.clear();
                RefreshAfterPresetChange(a_menu);
                statusMessage = StatusText("presetControlSaved");
            }
            else
            {
                if (!error.empty()) logger::warn("[Tuning Menu] preset changes save failed | profile={} | {}", profile, error);
                statusMessage = StatusText("presetControlSaveFailure");
            }
        }

        void RestorePresetControl(const MenuDefinition& a_menu)
        {
            auto profile = a_menu.profile;
            pendingPresetRemovals.erase(Lowercase(profile));
            auto& input = presetSaveInputs[profile];
            input.removalCategory.clear();
            input.removalPreset.clear();
            input.categoryRenameSource.clear();
            input.presetRenameSource.clear();
            input.settingsSelection.clear();
            RefreshAfterPresetChange(a_menu);
            statusMessage = StatusText("presetControlRestored");
        }

        std::string ActivePresetSignature(const std::span<const WeatherPatcher::ActivePreset> a_presets)
        {
            std::string signature;
            for (const auto& preset : a_presets)
            {
                signature.append(PresetVisualKey(preset.category, preset.name));
                signature.push_back('\x1E');
            }
            return signature;
        }

        const std::unordered_set<std::string>& DifferingActivePresets(
            std::string& a_profile,
            const std::span<const WeatherPatcher::ActivePreset> a_activePresets)
        {
            auto& state = presetVisualStates[Lowercase(a_profile)];
            const auto revision = TuningUtil::GetSettingsRevision();
            const auto signature = ActivePresetSignature(a_activePresets);
            if (state.initialized && state.settingsRevision == revision && state.activeSignature == signature)
            {
                return state.differingPresets;
            }

            state.settingsRevision = revision;
            state.activeSignature = signature;
            state.differingPresets.clear();
            state.initialized = true;

            const auto currentSettings = rfl::json::write<rfl::NoOptionals>(
                TuningUtil::GetSettings(a_profile),
                rfl::json::pretty);
            for (const auto& preset : a_activePresets)
            {
                const auto path = TuningUtil::ProfileDirectory(a_profile) / preset.category / (preset.name + ".json");
                std::ifstream file(path, std::ios::binary);
                if (!file)
                {
                    logger::warn("[Tuning Menu] preset comparison failed | preset={} | path={} unreadable", preset.name, path.string());
                    continue;
                }

                const std::string presetSettings(std::istreambuf_iterator<char>(file), {});
                std::string error;
                const auto declaredSettings = JsonOverlay::ProjectLike(presetSettings, currentSettings, error);
                const auto currentDeclaredSettings = declaredSettings ?
                                                         JsonOverlay::ProjectLike(currentSettings, *declaredSettings, error) :
                                                         std::nullopt;
                const auto equivalent = currentDeclaredSettings ?
                                            JsonOverlay::Equivalent(*currentDeclaredSettings, *declaredSettings, error) :
                                            std::nullopt;
                if (!equivalent)
                {
                    logger::warn("[Tuning Menu] preset comparison failed | preset={} | path={} | {}", preset.name, path.string(), error);
                    continue;
                }
                if (!*equivalent)
                {
                    state.differingPresets.insert(PresetVisualKey(preset.category, preset.name));
                }
            }
            return state.differingPresets;
        }

        void DrawPresetBrowser(const MenuDefinition& a_menu, const MenuControl& a_control)
        {
            auto profile = a_menu.profile;
            const auto& pending = PendingPresetRemovalState(profile);
            std::string activePresetError;
            const auto activePresets = WeatherPatcher::GetActivePresets(profile, activePresetError);
            const auto& differingPresets = DifferingActivePresets(profile, activePresets);
            auto drewCategory = false;
            for (const auto& category : WeatherPatcher::GetPresetCategories(profile))
            {
                if (pending.categories.contains(Lowercase(category))) continue;
                auto presets = WeatherPatcher::GetPresets(profile, category);
                std::erase_if(presets, [&](const std::string& a_preset)
                    { return pending.presets.contains(PresetVisualKey(category, a_preset)); });
                drewCategory = true;

                const auto panelId = category + "##" + profile + "PresetCategory";
                constexpr auto panelFlags = ImGuiMCP::ImGuiChildFlags_Border |
                                            ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                                            ImGuiMCP::ImGuiChildFlags_AutoResizeY;
                const auto padding = SKSEMenuSettings::GetBoxPadding();
                const auto customPadding = padding[0] > 0.0f || padding[1] > 0.0f;
                if (customPadding)
                {
                    ImGuiMCP::PushStyleVar(
                        ImGuiMCP::ImGuiStyleVar_WindowPadding,
                        ImGuiMCP::ImVec2(padding[0], padding[1]));
                }
                if (ImGuiMCP::BeginChild(panelId.c_str(), ImGuiMCP::ImVec2(0.0f, 0.0f), panelFlags))
                {
                    DrawHeader(category);
                    {
                        const auto presetSelected = std::ranges::any_of(
                            activePresets,
                            [&](const WeatherPatcher::ActivePreset& a_preset)
                            { return Config::IEquals(a_preset.category, category); });
                        const auto label = SKSEMenuSettings::Label("presetDefault", "Default") +
                                           "##" + profile + category + "PresetDefault";
                        auto selected = false;
                        if (presetSelected)
                        {
                            const ButtonColorStyle color(
                                SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::ordinary));
                            selected = ImGuiMCP::Button(label.c_str());
                        }
                        else
                        {
                            const auto presetColor = [&](const SKSEMenuSettings::InteractionState a_interaction)
                            {
                                const auto color = SKSEMenuSettings::GetPresetColor(
                                    SKSEMenuSettings::PresetState::active,
                                    a_interaction);
                                return ImGuiMCP::ImVec4(color[0], color[1], color[2], color[3]);
                            };
                            ImGuiMCP::PushStyleColor(
                                ImGuiMCP::ImGuiCol_Button,
                                presetColor(SKSEMenuSettings::InteractionState::normal));
                            ImGuiMCP::PushStyleColor(
                                ImGuiMCP::ImGuiCol_ButtonHovered,
                                presetColor(SKSEMenuSettings::InteractionState::hovered));
                            ImGuiMCP::PushStyleColor(
                                ImGuiMCP::ImGuiCol_ButtonActive,
                                presetColor(SKSEMenuSettings::InteractionState::pressed));
                            selected = ImGuiMCP::Button(label.c_str());
                            ImGuiMCP::PopStyleColor(3);
                        }
                        if (selected)
                        {
                            std::string error;
                            if (WeatherPatcher::PreviewPresetDefault(profile, category, error))
                            {
                                weatherMenuEntries.clear();
                                sliderCreatorWeatherEntries.clear();
                                activeWeatherLockProfile.clear();
                                ActivateWeatherLockPreference(a_menu);
                                statusMessage = StatusText(
                                    "presetDefaultPreview",
                                    { { "category", category } });
                            }
                            else
                            {
                                if (!error.empty()) logger::warn("[Tuning Menu] preset default preview failed | profile={} | category={} | {}", profile, category, error);
                                statusMessage = StatusText("presetLoadFailure");
                            }
                        }
                    }
                    if (presets.empty())
                    {
                        DrawDisplayText("noPresets", true);
                    }
                    for (const auto& preset : presets)
                    {
                        SameActionLine();

                        const auto label = preset + "##" + profile + category + "Preset";
                        const auto active = std::ranges::any_of(activePresets, [&](const WeatherPatcher::ActivePreset& a_preset)
                            { return Config::IEquals(a_preset.category, category) && Config::IEquals(a_preset.name, preset); });
                        const auto differs = active && differingPresets.contains(PresetVisualKey(category, preset));
                        if (active)
                        {
                            const auto presetColor = [&](const SKSEMenuSettings::InteractionState a_interaction)
                            {
                                const auto color = SKSEMenuSettings::GetPresetColor(
                                    differs ? SKSEMenuSettings::PresetState::modified : SKSEMenuSettings::PresetState::active,
                                    a_interaction);
                                return ImGuiMCP::ImVec4(
                                    color[0], color[1], color[2], color[3]);
                            };
                            ImGuiMCP::PushStyleColor(
                                ImGuiMCP::ImGuiCol_Button,
                                presetColor(SKSEMenuSettings::InteractionState::normal));
                            ImGuiMCP::PushStyleColor(
                                ImGuiMCP::ImGuiCol_ButtonHovered,
                                presetColor(SKSEMenuSettings::InteractionState::hovered));
                            ImGuiMCP::PushStyleColor(
                                ImGuiMCP::ImGuiCol_ButtonActive,
                                presetColor(SKSEMenuSettings::InteractionState::pressed));
                        }
                        const auto selected = ImGuiMCP::Button(label.c_str());
                        if (active)
                        {
                            ImGuiMCP::PopStyleColor(3);
                        }
                        if (selected)
                        {
                            std::string error;
                            if (WeatherPatcher::PreviewPreset(profile, category, preset, error))
                            {
                                weatherMenuEntries.clear();
                                sliderCreatorWeatherEntries.clear();
                                activeWeatherLockProfile.clear();
                                ActivateWeatherLockPreference(a_menu);
                                statusMessage = StatusText(
                                    "presetPreview",
                                    {
                                        { "category", category },
                                        { "preset", preset },
                                    });
                            }
                            else
                            {
                                if (!error.empty()) logger::warn("[Tuning Menu] preset preview failed | profile={} | category={} | preset={} | {}", profile, category, preset, error);
                                statusMessage = StatusText("presetLoadFailure");
                            }
                        }
                    }
                }
                ImGuiMCP::EndChild();
                if (customPadding) ImGuiMCP::PopStyleVar();
                ImGuiMCP::Spacing();
            }
            if (!drewCategory) DrawDisplayText("noPresets", true);
        }

        void DrawPresetCreator(const MenuDefinition& a_menu, const MenuControl& a_control)
        {
            auto profile = a_menu.profile;
            auto& input = presetSaveInputs[profile];
            auto& pending = PendingPresetRemovalState(profile);
            const auto moduleName = ControlLabel(a_control, "presetControl", "Presets Create");
            const auto heading = ControlDisplayName(
                a_control,
                a_control.header.empty() ?
                    SKSEMenuSettings::Label("createPresetHeader", "Create Preset") :
                    a_control.header);
            const auto savePanelId = moduleName + "##" + profile + "PresetCategory";
            constexpr auto savePanelFlags = ImGuiMCP::ImGuiChildFlags_Border |
                                            ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                                            ImGuiMCP::ImGuiChildFlags_AutoResizeY;
            const auto padding = SKSEMenuSettings::GetBoxPadding();
            const auto customPadding = padding[0] > 0.0f || padding[1] > 0.0f;
            if (customPadding)
            {
                ImGuiMCP::PushStyleVar(
                    ImGuiMCP::ImGuiStyleVar_WindowPadding,
                    ImGuiMCP::ImVec2(padding[0], padding[1]));
            }
            if (ImGuiMCP::BeginChild(savePanelId.c_str(), ImGuiMCP::ImVec2(0.0f, 0.0f), savePanelFlags))
            {
                if (!heading.empty()) DrawHeader(heading);
                {
                    const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
                    const auto label = SKSEMenuSettings::Label("savePresetControl", "Save Presets") +
                                       "##" + profile + "PresetControlSave";
                    if (ImGuiMCP::Button(label.c_str())) SavePresetControl(a_menu);
                }
                SameActionLine();
                {
                    const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::restore));
                    const auto label = SKSEMenuSettings::Label("restorePresetControl", "Restore Presets") +
                                       "##" + profile + "PresetControlRestore";
                    if (ImGuiMCP::Button(label.c_str())) RestorePresetControl(a_menu);
                }
                ImGuiMCP::Spacing();
                ImGuiMCP::SetNextItemWidth(280.0f);
                ImGuiMCP::InputTextWithHint(
                    ("Category##" + profile + "PresetCategoryInput").c_str(),
                    "Category folder",
                    input.category.data(),
                    input.category.size());
                ImGuiMCP::SetNextItemWidth(280.0f);
                ImGuiMCP::InputTextWithHint(
                    ("Preset Name##" + profile + "PresetNameInput").c_str(),
                    "Preset name",
                    input.name.data(),
                    input.name.size());
                const auto saveLabel = SKSEMenuSettings::Label("savePreset", "Save Preset");
                if (ImGuiMCP::Button((saveLabel + "##" + profile + "PresetSaveButton").c_str()))
                {
                    std::string error;
                    const std::string category(input.category.data());
                    const std::string presetName(input.name.data());
                    if (WeatherPatcher::SavePreset(profile, category, presetName, error))
                    {
                        pending.categories.erase(Lowercase(category));
                        pending.presets.erase(PresetVisualKey(category, presetName));
                        presetVisualStates.erase(Lowercase(profile));
                        input.settingsSelection.clear();
                        input.name.fill('\0');
                        statusMessage = StatusText(
                            "presetCreated",
                            {
                                { "preset", presetName },
                                { "category", category },
                            });
                    }
                    else
                    {
                        if (!error.empty()) logger::warn("[Tuning Menu] preset save failed | profile={} | category={} | preset={} | {}", profile, category, presetName, error);
                        statusMessage = StatusText("presetCreateFailure");
                    }
                }

                DrawHeader(SKSEMenuSettings::Label("editPresetHeader", "Edit Preset"));
                auto categories = WeatherPatcher::GetPresetCategories(profile);
                std::erase_if(categories, [&](const std::string& a_category)
                    { return pending.categories.contains(Lowercase(a_category)); });
                if (!std::ranges::contains(categories, input.removalCategory))
                {
                    input.removalCategory.clear();
                    input.removalPreset.clear();
                    input.categoryRenameSource.clear();
                    input.presetRenameSource.clear();
                    input.settingsSelection.clear();
                }
                const auto categoryPreview = input.removalCategory.empty() ?
                                                 DisplayText("selectPresetCategory") :
                                                 input.removalCategory;
                const auto categorySelectionLabel = SKSEMenuSettings::Label("presetCategory", "Category") +
                                                    "##" + profile + "PresetEditCategory";
                if (ImGuiMCP::BeginCombo(categorySelectionLabel.c_str(), categoryPreview.c_str()))
                {
                    for (const auto& category : categories)
                    {
                        if (ImGuiMCP::Selectable(category.c_str(), Config::IEquals(category, input.removalCategory)))
                        {
                            input.removalCategory = category;
                            input.removalPreset.clear();
                            input.categoryRenameSource.clear();
                            input.presetRenameSource.clear();
                            input.settingsSelection.clear();
                        }
                    }
                    ImGuiMCP::EndCombo();
                }

                std::vector<std::string> presets;
                if (!input.removalCategory.empty())
                {
                    presets = WeatherPatcher::GetPresets(profile, input.removalCategory);
                    std::erase_if(presets, [&](const std::string& a_preset)
                        { return pending.presets.contains(PresetVisualKey(input.removalCategory, a_preset)); });
                }
                if (!std::ranges::contains(presets, input.removalPreset)) input.removalPreset.clear();
                const auto presetPreview = input.removalPreset.empty() ? DisplayText("selectPreset") : input.removalPreset;
                ImGuiMCP::BeginDisabled(input.removalCategory.empty());
                const auto presetSelectionLabel = SKSEMenuSettings::Label("preset", "Preset") +
                                                  "##" + profile + "PresetEditName";
                if (ImGuiMCP::BeginCombo(presetSelectionLabel.c_str(), presetPreview.c_str()))
                {
                    for (const auto& preset : presets)
                    {
                        if (ImGuiMCP::Selectable(preset.c_str(), Config::IEquals(preset, input.removalPreset)))
                        {
                            input.removalPreset = preset;
                            input.presetRenameSource.clear();
                            input.settingsSelection.clear();
                        }
                    }
                    ImGuiMCP::EndCombo();
                }
                ImGuiMCP::EndDisabled();

                const auto selectedPreset = std::ranges::find_if(presets, [&](const auto& a_preset)
                    { return Config::IEquals(a_preset, input.removalPreset); });
                const auto selectedPresetIndex = selectedPreset == presets.end() ?
                                                     presets.size() :
                                                     static_cast<std::size_t>(std::distance(presets.begin(), selectedPreset));
                ImGuiMCP::BeginDisabled(selectedPresetIndex == 0 || selectedPresetIndex >= presets.size());
                const auto movePresetUpLabel = SKSEMenuSettings::Label("movePresetUp", "Move Up") +
                                               "##" + profile;
                if (ImGuiMCP::Button(movePresetUpLabel.c_str()))
                {
                    std::string error;
                    if (WeatherPatcher::MovePreset(
                            profile,
                            input.removalCategory,
                            input.removalPreset,
                            -1,
                            error))
                    {
                        RefreshAfterPresetChange(a_menu);
                        statusMessage = StatusText("presetMoved");
                    }
                    else
                    {
                        if (!error.empty()) logger::warn("[Tuning Menu] preset move failed | profile={} | category={} | preset={} | direction=up | {}", profile, input.removalCategory, input.removalPreset, error);
                        statusMessage = StatusText("presetMoveFailure");
                    }
                }
                ImGuiMCP::EndDisabled();
                SameActionLine();
                ImGuiMCP::BeginDisabled(
                    selectedPresetIndex >= presets.size() || selectedPresetIndex + 1 >= presets.size());
                const auto movePresetDownLabel = SKSEMenuSettings::Label("movePresetDown", "Move Down") +
                                                 "##" + profile;
                if (ImGuiMCP::Button(movePresetDownLabel.c_str()))
                {
                    std::string error;
                    if (WeatherPatcher::MovePreset(
                            profile,
                            input.removalCategory,
                            input.removalPreset,
                            1,
                            error))
                    {
                        RefreshAfterPresetChange(a_menu);
                        statusMessage = StatusText("presetMoved");
                    }
                    else
                    {
                        if (!error.empty()) logger::warn("[Tuning Menu] preset move failed | profile={} | category={} | preset={} | direction=down | {}", profile, input.removalCategory, input.removalPreset, error);
                        statusMessage = StatusText("presetMoveFailure");
                    }
                }
                ImGuiMCP::EndDisabled();

                if (input.categoryRenameSource != input.removalCategory)
                {
                    SetInputText(input.categoryRename, input.removalCategory);
                    input.categoryRenameSource = input.removalCategory;
                }
                const auto presetRenameSource = input.removalPreset.empty() ?
                                                    std::string{} :
                                                    PresetVisualKey(input.removalCategory, input.removalPreset);
                if (input.presetRenameSource != presetRenameSource)
                {
                    SetInputText(input.presetRename, input.removalPreset);
                    input.presetRenameSource = presetRenameSource;
                }

                ImGuiMCP::SetNextItemWidth(280.0f);
                const auto categoryNameLabel = SKSEMenuSettings::Label("presetCategoryName", "Category Name") +
                                               "##" + profile + "PresetCategoryRename";
                ImGuiMCP::InputText(
                    categoryNameLabel.c_str(),
                    input.categoryRename.data(),
                    input.categoryRename.size());
                const auto renamedCategoryInput = Trim(std::string(input.categoryRename.data()));
                ImGuiMCP::BeginDisabled(
                    input.removalCategory.empty() ||
                    renamedCategoryInput.empty() ||
                    renamedCategoryInput == input.removalCategory);
                const auto renameCategoryLabel = SKSEMenuSettings::Label("renamePresetCategory", "Rename Category") +
                                                 "##" + profile;
                if (ImGuiMCP::Button(renameCategoryLabel.c_str()))
                {
                    const auto previousCategory = input.removalCategory;
                    std::string error;
                    if (WeatherPatcher::RenamePresetCategory(
                            profile,
                            previousCategory,
                            renamedCategoryInput,
                            error))
                    {
                        const auto renamedCategories = WeatherPatcher::GetPresetCategories(profile);
                        const auto renamed = std::ranges::find_if(renamedCategories, [&](const auto& a_category)
                            { return Config::IEquals(a_category, renamedCategoryInput); });
                        const auto renamedCategory = renamed != renamedCategories.end() ?
                                                         *renamed :
                                                         renamedCategoryInput;
                        decltype(pending.presets) renamedPendingPresets;
                        for (auto& [key, pendingPreset] : pending.presets)
                        {
                            (void)key;
                            if (Config::IEquals(pendingPreset.category, previousCategory))
                                pendingPreset.category = renamedCategory;
                            renamedPendingPresets.insert_or_assign(
                                PresetVisualKey(pendingPreset.category, pendingPreset.name),
                                std::move(pendingPreset));
                        }
                        pending.presets = std::move(renamedPendingPresets);
                        pending.categories.erase(Lowercase(previousCategory));
                        input.removalCategory = renamedCategory;
                        input.categoryRenameSource.clear();
                        input.presetRenameSource.clear();
                        input.settingsSelection.clear();
                        RefreshAfterPresetChange(a_menu);
                        statusMessage = StatusText(
                            "presetCategoryRenamed",
                            {
                                { "category", previousCategory },
                                { "newCategory", renamedCategory },
                            });
                    }
                    else
                    {
                        if (!error.empty()) logger::warn("[Tuning Menu] preset category rename failed | profile={} | category={} | {}", profile, previousCategory, error);
                        statusMessage = StatusText("presetCategoryRenameFailure");
                    }
                }
                ImGuiMCP::EndDisabled();
                SameActionLine();
                ImGuiMCP::BeginDisabled(input.removalCategory.empty());
                {
                    const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                    const auto removeCategoryLabel = SKSEMenuSettings::Label("removePresetCategory", "Remove Category") +
                                                     "##" + profile;
                    if (ImGuiMCP::Button(removeCategoryLabel.c_str()))
                    {
                        const auto category = input.removalCategory;
                        pending.categories.insert_or_assign(Lowercase(category), category);
                        std::erase_if(pending.presets, [&](const auto& a_entry)
                            { return Config::IEquals(a_entry.second.category, category); });
                        input.removalCategory.clear();
                        input.removalPreset.clear();
                        input.categoryRenameSource.clear();
                        input.presetRenameSource.clear();
                        input.settingsSelection.clear();
                        RefreshAfterPresetChange(a_menu);
                        statusMessage = StatusText("presetCategoryRemovalStaged", { { "category", category } });
                    }
                }
                ImGuiMCP::EndDisabled();

                ImGuiMCP::SetNextItemWidth(280.0f);
                const auto presetNameLabel = SKSEMenuSettings::Label("presetName", "Preset Name") +
                                             "##" + profile + "PresetRename";
                ImGuiMCP::InputText(
                    presetNameLabel.c_str(),
                    input.presetRename.data(),
                    input.presetRename.size());
                auto renamedPresetInput = Trim(std::string(input.presetRename.data()));
                if (Config::IEquals(std::filesystem::path(renamedPresetInput).extension().string(), ".json"))
                    renamedPresetInput = std::filesystem::path(renamedPresetInput).stem().string();
                ImGuiMCP::BeginDisabled(
                    input.removalCategory.empty() ||
                    input.removalPreset.empty() ||
                    renamedPresetInput.empty() ||
                    renamedPresetInput == input.removalPreset);
                const auto renamePresetLabel = SKSEMenuSettings::Label("renamePreset", "Rename Preset") +
                                               "##" + profile;
                if (ImGuiMCP::Button(renamePresetLabel.c_str()))
                {
                    const auto previousPreset = input.removalPreset;
                    std::string error;
                    if (WeatherPatcher::RenamePreset(
                            profile,
                            input.removalCategory,
                            previousPreset,
                            renamedPresetInput,
                            error))
                    {
                        const auto renamedPresets = WeatherPatcher::GetPresets(profile, input.removalCategory);
                        const auto renamed = std::ranges::find_if(renamedPresets, [&](const auto& a_preset)
                            { return Config::IEquals(a_preset, renamedPresetInput); });
                        const auto renamedPreset = renamed != renamedPresets.end() ? *renamed : renamedPresetInput;
                        input.removalPreset = renamedPreset;
                        input.presetRenameSource.clear();
                        input.settingsSelection.clear();
                        RefreshAfterPresetChange(a_menu);
                        statusMessage = StatusText(
                            "presetRenamed",
                            {
                                { "preset", previousPreset },
                                { "newPreset", renamedPreset },
                                { "category", input.removalCategory },
                            });
                    }
                    else
                    {
                        if (!error.empty()) logger::warn("[Tuning Menu] preset rename failed | profile={} | category={} | preset={} | {}", profile, input.removalCategory, previousPreset, error);
                        statusMessage = StatusText("presetRenameFailure");
                    }
                }
                ImGuiMCP::EndDisabled();
                SameActionLine();
                ImGuiMCP::BeginDisabled(input.removalCategory.empty() || input.removalPreset.empty());
                {
                    const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
                    const auto updatePresetLabel = SKSEMenuSettings::Label("updatePreset", "Update Preset") +
                                                   "##" + profile;
                    if (ImGuiMCP::Button(updatePresetLabel.c_str()))
                    {
                        std::string error;
                        if (WeatherPatcher::UpdatePreset(
                                profile,
                                input.removalCategory,
                                input.removalPreset,
                                error))
                        {
                            input.settingsSelection.clear();
                            RefreshAfterPresetChange(a_menu);
                            statusMessage = StatusText(
                                "presetUpdated",
                                {
                                    { "preset", input.removalPreset },
                                    { "category", input.removalCategory },
                                });
                        }
                        else
                        {
                            if (!error.empty()) logger::warn("[Tuning Menu] preset update failed | profile={} | category={} | preset={} | {}", profile, input.removalCategory, input.removalPreset, error);
                            statusMessage = StatusText("presetUpdateFailure");
                        }
                    }
                }
                SameActionLine();
                {
                    const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                    const auto removePresetLabel = SKSEMenuSettings::Label("removePreset", "Remove Preset") +
                                                   "##" + profile;
                    if (ImGuiMCP::Button(removePresetLabel.c_str()))
                    {
                        const auto category = input.removalCategory;
                        const auto preset = input.removalPreset;
                        pending.presets.insert_or_assign(PresetVisualKey(category, preset), WeatherPatcher::ActivePreset{ category, preset });
                        input.removalPreset.clear();
                        input.presetRenameSource.clear();
                        input.settingsSelection.clear();
                        RefreshAfterPresetChange(a_menu);
                        statusMessage = StatusText("presetRemovalStaged", { { "preset", preset }, { "category", category } });
                    }
                }
                ImGuiMCP::EndDisabled();

                DrawHeader(SKSEMenuSettings::Label("presetSettings", "Preset Settings"));
                const auto settingsSelection = input.removalPreset.empty() ?
                                                   std::string{} :
                                                   PresetVisualKey(input.removalCategory, input.removalPreset);
                if (input.settingsSelection != settingsSelection)
                {
                    input.settingsSelection = settingsSelection;
                    input.settings.clear();
                    input.settingsError.clear();
                    if (!settingsSelection.empty())
                    {
                        const auto settings = WeatherPatcher::GetPresetSettings(
                            profile,
                            input.removalCategory,
                            input.removalPreset,
                            input.settingsError);
                        const auto values = settings ?
                                                JsonOverlay::FlattenValues(*settings, input.settingsError) :
                                                std::nullopt;
                        if (values) input.settings = std::move(*values);
                    }
                }

                if (settingsSelection.empty())
                {
                    DrawDisplayText("selectPresetSettings", true);
                }
                else if (!input.settingsError.empty())
                {
                    DrawDisplayText("presetSettingsLoadFailure", true);
                }
                else if (input.settings.empty())
                {
                    DrawDisplayText("noPresetSettings", true);
                }
                else
                {
                    constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_SizingStretchProp |
                                                ImGuiMCP::ImGuiTableFlags_RowBg |
                                                ImGuiMCP::ImGuiTableFlags_BordersInnerH |
                                                ImGuiMCP::ImGuiTableFlags_BordersOuter;
                    const auto tableID = "PresetSettings##" + profile;
                    if (ImGuiMCP::BeginTable(tableID.c_str(), 2, tableFlags))
                    {
                        ImGuiMCP::TableSetupColumn(
                            SKSEMenuSettings::Label("presetSetting", "Setting").c_str(),
                            ImGuiMCP::ImGuiTableColumnFlags_WidthStretch,
                            0.6f);
                        ImGuiMCP::TableSetupColumn(
                            SKSEMenuSettings::Label("presetValue", "Value").c_str(),
                            ImGuiMCP::ImGuiTableColumnFlags_WidthStretch,
                            0.4f);
                        ImGuiMCP::TableHeadersRow();
                        for (const auto& setting : input.settings)
                        {
                            ImGuiMCP::TableNextRow();
                            ImGuiMCP::TableSetColumnIndex(0);
                            ImGuiMCP::TextWrapped("%s", setting.path.c_str());
                            ImGuiMCP::TableSetColumnIndex(1);
                            ImGuiMCP::TextWrapped("%s", setting.value.c_str());
                        }
                        ImGuiMCP::EndTable();
                    }
                }
            }
            ImGuiMCP::EndChild();
            if (customPadding) ImGuiMCP::PopStyleVar();
        }

        void DrawPresets(const MenuDefinition& a_menu, const MenuControl& a_control)
        {
            DrawPresetBrowser(a_menu, a_control);
        }

        template <std::size_t Size>
        void SetInputText(std::array<char, Size>& a_buffer, const std::string_view a_value)
        {
            a_buffer.fill('\0');
            const auto length = std::min(a_value.size(), Size - 1);
            std::memcpy(a_buffer.data(), a_value.data(), length);
        }

        template <std::size_t Size>
        std::string InputText(const std::array<char, Size>& a_buffer)
        {
            return Trim(std::string(a_buffer.data()));
        }

        void ResetSliderCreator(
            SliderCreatorState& a_state,
            std::string a_profile,
            const std::size_t a_pageIndex,
            const SliderCreatorDomain a_domain)
        {
            a_state = {};
            a_state.profile = std::move(a_profile);
            a_state.pageIndex = a_pageIndex;
            a_state.domain = a_domain;
            a_state.filtered = a_domain == SliderCreatorDomain::weather;
            a_state.defaultValue = 1.0f;
            a_state.minimum = 0.0f;
            a_state.maximum = 2.0f;
            a_state.step = 0.1f;
            a_state.initialized = true;
        }

        void LoadSliderCreatorDefinition(
            SliderCreatorState& a_state,
            const std::size_t a_pageIndex,
            const SliderCreator::ExistingSlider& a_slider)
        {
            const auto profile = a_state.profile;
            const auto& definition = a_slider.definition;
            auto domain = definition.filtered && definition.filterDomain == SliderCreator::FilterDomain::weather ?
                              SliderCreatorDomain::weather :
                          definition.filtered ?
                              SliderCreatorDomain::interior :
                              a_state.domain;
            for (const auto& target : definition.settings)
            {
                const auto* entry = SliderSettingCatalog::Find(target.setting);
                if (!entry) continue;
                domain = entry->domain == SliderSettingCatalog::Domain::weather ?
                             SliderCreatorDomain::weather :
                             SliderCreatorDomain::interior;
                break;
            }
            ResetSliderCreator(
                a_state,
                profile,
                a_state.pageIndex,
                domain);
            a_state.loadedPageIndex = a_pageIndex;
            a_state.loadedControlIndex = a_slider.controlIndex;
            a_state.loadedSliderID = definition.id;
            SetInputText(a_state.label, definition.label);
            SetInputText(a_state.tooltip, definition.tooltip);
            SetInputText(a_state.link, definition.link);
            SetInputText(a_state.localLink, definition.localLink);
            SetInputText(a_state.format, definition.format);
            a_state.settings = definition.settings;
            a_state.include = definition.include;
            a_state.exclude = definition.exclude;
            a_state.useHueScales = definition.hueScales.has_value();
            if (definition.hueScales)
            {
                const auto& value = *definition.hueScales;
                a_state.hueScales = {
                    static_cast<float>(value.red),
                    static_cast<float>(value.orange),
                    static_cast<float>(value.yellow),
                    static_cast<float>(value.green),
                    static_cast<float>(value.teal),
                    static_cast<float>(value.blue),
                    static_cast<float>(value.magenta),
                };
            }
            a_state.filtered = definition.filtered;
            a_state.invert = definition.invert;
            a_state.useTimes = definition.useTimes;
            a_state.times = definition.times;
            a_state.useDefault = definition.defaultValue.has_value();
            a_state.defaultValue = static_cast<float>(definition.defaultValue.value_or(1.0));
            a_state.useRange = definition.minimum || definition.maximum || definition.step;
            a_state.minimum = static_cast<float>(definition.minimum.value_or(0.0));
            a_state.maximum = static_cast<float>(definition.maximum.value_or(2.0));
            a_state.step = static_cast<float>(definition.step.value_or(0.1));
            a_state.useWidth = definition.width.has_value();
            a_state.width = static_cast<float>(definition.width.value_or(0.0));
            if (!definition.settings.empty())
            {
                if (const auto* entry =
                        SliderSettingCatalog::Find(
                            definition.settings.front().setting))
                {
                    const auto catalogDomain =
                        entry->domain;
                    const auto groups =
                        SliderSettingCatalog::Groups(catalogDomain);
                    const auto group = std::ranges::find_if(
                        groups,
                        [&](const auto a_group)
                        { return Config::IEquals(a_group, entry->group); });
                    if (group != groups.end())
                    {
                        a_state.catalogGroup =
                            static_cast<int>(
                                std::distance(groups.begin(), group));
                        const auto entries =
                            SliderSettingCatalog::Entries(
                                catalogDomain,
                                *group);
                        const auto value = std::ranges::find(
                            entries,
                            entry);
                        if (value != entries.end())
                        {
                            a_state.catalogSetting =
                                static_cast<int>(
                                    std::distance(
                                        entries.begin(),
                                        value));
                        }
                    }
                }
            }
        }

        std::string MakeSliderID(const std::string_view a_label)
        {
            std::string result;
            auto capitalize = false;
            for (const unsigned char character : a_label)
            {
                if (std::isalnum(character) == 0)
                {
                    capitalize = !result.empty();
                    continue;
                }
                auto output = static_cast<char>(character);
                if (result.empty()) output = static_cast<char>(std::tolower(character));
                else if (capitalize) output = static_cast<char>(std::toupper(character));
                result.push_back(output);
                capitalize = false;
            }
            return result;
        }

        bool AddUniqueString(std::vector<std::string>& a_values, std::string a_value)
        {
            a_value = Trim(std::move(a_value));
            if (a_value.empty() || std::ranges::any_of(a_values, [&](const auto& a_existing)
                                       { return Config::IEquals(a_existing, a_value); }))
                return false;
            a_values.push_back(std::move(a_value));
            return true;
        }

        void DrawCreatorTextList(
            std::vector<std::string>& a_values,
            std::array<char, 96>& a_input,
            int& a_selection,
            const std::string& a_id,
            const std::string_view a_inputLabel,
            const std::string_view a_inputHint,
            const std::string_view a_updateLabel,
            const std::string_view a_clearLabel,
            const std::string_view a_listLabel,
            const std::string_view a_removeLabel,
            const std::string_view a_emptyMessage)
        {
            ImGuiMCP::SetNextItemWidth(280.0f);
            ImGuiMCP::InputTextWithHint(
                (std::string(a_inputLabel) + "##" + a_id).c_str(),
                a_inputHint.data(),
                a_input.data(),
                a_input.size());
            const auto updateLabel = std::string(a_updateLabel) + "##" + a_id;
            if (ImGuiMCP::Button(updateLabel.c_str()))
            {
                auto value = InputText(a_input);
                if (!value.empty())
                {
                    if (a_selection >= 0 && static_cast<std::size_t>(a_selection) < a_values.size())
                    {
                        const auto duplicate = std::ranges::any_of(a_values, [&](const auto& a_existing)
                            { return &a_existing != std::addressof(a_values[static_cast<std::size_t>(a_selection)]) &&
                                     Config::IEquals(a_existing, value); });
                        if (!duplicate) a_values[static_cast<std::size_t>(a_selection)] = std::move(value);
                    }
                    else
                    {
                        AddUniqueString(a_values, std::move(value));
                    }
                }
                a_selection = -1;
                a_input.fill('\0');
            }
            SameActionLine();
            const auto clearLabel = std::string(a_clearLabel) + "##" + a_id;
            ImGuiMCP::BeginDisabled(a_values.empty());
            if (ImGuiMCP::Button(clearLabel.c_str()))
            {
                a_values.clear();
                a_selection = -1;
                a_input.fill('\0');
            }
            ImGuiMCP::EndDisabled();

            if (ImGuiMCP::BeginListBox((std::string(a_listLabel) + "##" + a_id).c_str(), ImGuiMCP::ImVec2(0.0f, 110.0f)))
            {
                if (a_values.empty()) DrawDisplayText(a_emptyMessage, true);
                for (std::size_t index = 0; index < a_values.size();)
                {
                    const auto label = a_values[index] + "##" + a_id + std::to_string(index);
                    if (ImGuiMCP::Selectable(label.c_str(), a_selection == static_cast<int>(index)))
                    {
                        a_selection = static_cast<int>(index);
                        SetInputText(a_input, a_values[index]);
                    }
                    auto remove = false;
                    if (ImGuiMCP::BeginPopupContextItem(("ContainsContext##" + a_id + std::to_string(index)).c_str()))
                    {
                        remove = ImGuiMCP::MenuItem(
                            a_removeLabel.data());
                        ImGuiMCP::EndPopup();
                    }
                    if (remove)
                    {
                        a_values.erase(a_values.begin() + static_cast<std::ptrdiff_t>(index));
                        a_selection = -1;
                        a_input.fill('\0');
                    }
                    else ++index;
                }
                ImGuiMCP::EndListBox();
            }
        }

        void DrawCreatorContainsList(
            std::vector<std::string>& a_values,
            std::array<char, 96>& a_input,
            int& a_selection,
            const std::string& a_id)
        {
            DrawCreatorTextList(
                a_values,
                a_input,
                a_selection,
                a_id,
                SKSEMenuSettings::Label("contains", "Contains"),
                SKSEMenuSettings::Label("containsHint", "EditorID contains text"),
                SKSEMenuSettings::Label("addOrUpdateContains", "Add / Update Contains"),
                SKSEMenuSettings::Label("clearContains", "Clear Contains"),
                SKSEMenuSettings::Label("containsList", "Contains List"),
                SKSEMenuSettings::Label("removeContains", "Remove Contains"),
                "sliderCreatorNoContains");
        }

        void DrawCreatorPluginList(
            std::vector<std::string>& a_values,
            std::array<char, 96>& a_input,
            int& a_selection,
            const std::string& a_id)
        {
            DrawCreatorTextList(
                a_values,
                a_input,
                a_selection,
                a_id,
                SKSEMenuSettings::Label("pluginName", "Plugin"),
                SKSEMenuSettings::Label("pluginNameHint", "Full plugin filename, such as Skyrim.esm"),
                SKSEMenuSettings::Label("addOrUpdatePlugin", "Add / Update Plugin"),
                SKSEMenuSettings::Label("clearPlugins", "Clear Plugins"),
                SKSEMenuSettings::Label("pluginList", "Plugin List"),
                SKSEMenuSettings::Label("removePlugin", "Remove Plugin"),
                "noPlugins");
        }

        void DrawCreatorPluginContainsList(
            std::vector<std::string>& a_values,
            std::array<char, 96>& a_input,
            int& a_selection,
            const std::string& a_id)
        {
            DrawCreatorTextList(
                a_values,
                a_input,
                a_selection,
                a_id,
                SKSEMenuSettings::Label("contains", "Contains"),
                SKSEMenuSettings::Label("pluginContainsHint", "Plugin filename contains text"),
                SKSEMenuSettings::Label("addOrUpdateContains", "Add / Update Contains"),
                SKSEMenuSettings::Label("clearContains", "Clear Contains"),
                SKSEMenuSettings::Label("containsList", "Contains List"),
                SKSEMenuSettings::Label("removeContains", "Remove Contains"),
                "sliderCreatorNoContains");
        }

        void DrawCreatorWeatherList(
            std::vector<std::string>& a_values,
            const std::string& a_id)
        {
            if (ImGuiMCP::BeginListBox(a_id.c_str(), ImGuiMCP::ImVec2(0.0f, 150.0f)))
            {
                if (a_values.empty()) DrawDisplayText("sliderCreatorNoWeathers", true);
                for (std::size_t index = 0; index < a_values.size();)
                {
                    auto* weather = Config::LiteForm::FromString(a_values[index]).Get<RE::TESWeather>();
                    const auto name = WeatherDisplayLabel(weather);
                    const auto label = name + "##" + a_id + std::to_string(index);
                    ImGuiMCP::Selectable(label.c_str(), false);
                    auto remove = false;
                    if (ImGuiMCP::BeginPopupContextItem(("WeatherFilterContext##" + a_id + std::to_string(index)).c_str()))
                    {
                        remove = ImGuiMCP::MenuItem(
                            SKSEMenuSettings::Label("removeFilteredWeather", "Remove Weather").c_str());
                        ImGuiMCP::EndPopup();
                    }
                    if (remove) a_values.erase(a_values.begin() + static_cast<std::ptrdiff_t>(index));
                    else ++index;
                }
                ImGuiMCP::EndListBox();
            }
        }

        void DrawCreatorRecordList(
            std::vector<std::string>& a_values,
            const std::string& a_id,
            const RecordFilterKind a_kind)
        {
            if (ImGuiMCP::BeginListBox(a_id.c_str(), ImGuiMCP::ImVec2(0.0f, 150.0f)))
            {
                if (a_values.empty()) DrawDisplayText("noRecords", true);
                for (std::size_t index = 0; index < a_values.size();)
                {
                    const auto form = Config::LiteForm::FromString(a_values[index]);
                    auto* record = [&]() -> RE::TESForm*
                    {
                        switch (a_kind)
                        {
                        case RecordFilterKind::lightingTemplate:
                            return form.Get<RE::BGSLightingTemplate>();
                        case RecordFilterKind::baseLight:
                            return form.Get<RE::TESObjectLIGH>();
                        case RecordFilterKind::effectLighting:
                            return form.Get<RE::TESWeather>();
                        }
                        return nullptr;
                    }();
                    const auto name = record ? RecordFilter::DisplayName(record) : a_values[index];
                    const auto label = name + "##" + a_id + std::to_string(index);
                    ImGuiMCP::Selectable(label.c_str(), false);
                    auto remove = false;
                    if (ImGuiMCP::BeginPopupContextItem(("RecordFilterContext##" + a_id + std::to_string(index)).c_str()))
                    {
                        remove = ImGuiMCP::MenuItem(
                            SKSEMenuSettings::Label("removeRecord", "Remove Record").c_str());
                        ImGuiMCP::EndPopup();
                    }
                    if (remove) a_values.erase(a_values.begin() + static_cast<std::ptrdiff_t>(index));
                    else ++index;
                }
                ImGuiMCP::EndListBox();
            }
        }

        SliderSettingCatalog::Domain CreatorCatalogDomain(const SliderCreatorState& a_state)
        {
            return a_state.domain == SliderCreatorDomain::weather ?
                       SliderSettingCatalog::Domain::weather :
                       SliderSettingCatalog::Domain::lighting;
        }

        void AddCreatorSetting(SliderCreatorState& a_state, const std::string_view a_setting)
        {
            if (!std::ranges::any_of(a_state.settings, [&](const auto& a_existing)
                    { return Config::IEquals(a_existing.setting, a_setting); }))
                a_state.settings.push_back({ std::string(a_setting), a_state.pendingScale, a_state.pendingIgnoreLink });
        }

        std::optional<SliderSettingCatalog::FilterOperation> CreatorFilteredOperation(
            const SliderCreatorState& a_state)
        {
            std::optional<SliderSettingCatalog::FilterOperation> result;
            for (const auto& target : a_state.settings)
            {
                const auto* entry = SliderSettingCatalog::Find(target.setting);
                if (!entry || !SliderSettingCatalog::IsFilteredOperation(entry->filterOperation) ||
                    (result && *result != entry->filterOperation))
                    return std::nullopt;
                result = entry->filterOperation;
            }
            return result;
        }

        bool CreatorUsesEffectLightingWeatherFilter(const SliderCreatorState& a_state)
        {
            return !a_state.settings.empty() &&
                   std::ranges::all_of(
                       a_state.settings,
                       [](const auto& a_setting)
                       {
                           const auto* entry = SliderSettingCatalog::Find(a_setting.setting);
                           return entry && entry->domain == SliderSettingCatalog::Domain::lighting &&
                                  entry->path.starts_with("fxEffectLighting.");
                       });
        }

        bool CreatorUsesBaseLightFilter(const SliderCreatorState& a_state)
        {
            return !a_state.settings.empty() &&
                   std::ranges::all_of(
                       a_state.settings,
                       [](const auto& a_setting)
                       {
                           const auto* entry = SliderSettingCatalog::Find(a_setting.setting);
                           return entry && entry->domain == SliderSettingCatalog::Domain::lighting &&
                                  entry->path.starts_with("pointLights.");
                       });
        }

        bool CreatorUsesWeatherFilter(const SliderCreatorState& a_state)
        {
            return a_state.domain == SliderCreatorDomain::weather ||
                   CreatorUsesEffectLightingWeatherFilter(a_state);
        }

        std::string CatalogLabelPart(const std::string_view a_part)
        {
            static constexpr std::array hues{ "red", "orange", "yellow", "green", "teal", "blue", "magenta" };
            const auto key = Lowercase(std::string(a_part));
            return std::ranges::contains(hues, std::string_view(key)) ?
                       SKSEMenuSettings::SettingHueLabel(key, a_part) :
                       SKSEMenuSettings::SettingNameLabel(a_part, a_part);
        }

        std::string CatalogSettingLabel(const SliderSettingCatalog::Entry& a_entry)
        {
            constexpr std::string_view separator = " / ";
            const auto position = a_entry.label.find(separator);
            std::string result;
            if (position != std::string::npos)
            {
                result = CatalogLabelPart(std::string_view(a_entry.label).substr(0, position)) +
                         SKSEMenuSettings::SettingLabelSeparator() +
                         CatalogLabelPart(std::string_view(a_entry.label).substr(position + separator.size()));
            }
            else
            {
                const auto suffix = a_entry.label.ends_with(" Start") ? std::string_view("Start") :
                                    a_entry.label.ends_with(" End")   ? std::string_view("End") :
                                                                        std::string_view{};
                if (!suffix.empty())
                {
                    const auto hue = std::string_view(a_entry.label).substr(0, a_entry.label.size() - suffix.size() - 1);
                    result = CatalogLabelPart(hue) + " " + SKSEMenuSettings::SettingNameLabel(suffix, suffix);
                }
                else
                {
                    result = CatalogLabelPart(a_entry.label);
                }
            }
            return SKSEMenuSettings::SettingPathLabel(a_entry.path, result);
        }

        void DrawSliderCreatorSettings(SliderCreatorState& a_state, const std::string& a_id)
        {
            static constexpr std::array categories{
                SliderCreatorDomain::interior,
                SliderCreatorDomain::weather,
            };
            const auto categoryName = [](const SliderCreatorDomain a_domain)
            {
                return a_domain == SliderCreatorDomain::weather ?
                           SKSEMenuSettings::Label(
                               "sliderCreatorWeather",
                               "Weather") :
                           SKSEMenuSettings::Label(
                               "sliderCreatorLighting",
                               "Lighting");
            };
            const auto categoryLabel =
                SKSEMenuSettings::Label(
                    "sliderCreatorCategory",
                    "Category");
            const auto categoryPreview = categoryName(a_state.domain);
            if (ImGuiMCP::BeginCombo(
                    (categoryLabel + "##" + a_id).c_str(),
                    categoryPreview.c_str()))
            {
                for (const auto category : categories)
                {
                    const auto label = categoryName(category);
                    const auto selected = category == a_state.domain;
                    if (ImGuiMCP::Selectable(
                            label.c_str(),
                            selected) &&
                        !selected)
                    {
                        a_state.domain = category;
                        a_state.catalogGroup = 0;
                        a_state.catalogSetting = 0;
                        a_state.filtered =
                            category ==
                            SliderCreatorDomain::weather;
                    }
                    if (selected) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndCombo();
            }

            const auto domain = CreatorCatalogDomain(a_state);
            const auto groups = SliderSettingCatalog::Groups(domain);
            if (groups.empty()) return;
            a_state.catalogGroup = std::clamp(a_state.catalogGroup, 0, static_cast<int>(groups.size() - 1));
            const auto groupPreview = SKSEMenuSettings::SettingGroupLabel(
                groups[a_state.catalogGroup], groups[a_state.catalogGroup]);
            const auto settingLabel =
                SKSEMenuSettings::Label(
                    "sliderCreatorSetting",
                    "Setting");
            if (ImGuiMCP::BeginCombo(
                    (settingLabel + "##" + a_id).c_str(),
                    groupPreview.c_str()))
            {
                for (std::size_t index = 0; index < groups.size(); ++index)
                {
                    const auto label = SKSEMenuSettings::SettingGroupLabel(groups[index], groups[index]);
                    if (ImGuiMCP::Selectable(label.c_str(), a_state.catalogGroup == static_cast<int>(index)))
                    {
                        a_state.catalogGroup = static_cast<int>(index);
                        a_state.catalogSetting = 0;
                    }
                }
                ImGuiMCP::EndCombo();
            }

            const auto entries = SliderSettingCatalog::Entries(domain, groups[a_state.catalogGroup]);
            if (entries.empty()) return;
            a_state.catalogSetting = std::clamp(a_state.catalogSetting, 0, static_cast<int>(entries.size() - 1));
            const auto valueLabel =
                SKSEMenuSettings::Label(
                    "sliderCreatorValue",
                    "Value");
            const auto settingPreview = CatalogSettingLabel(*entries[a_state.catalogSetting]);
            if (ImGuiMCP::BeginCombo(
                    (valueLabel + "##" + a_id).c_str(),
                    settingPreview.c_str()))
            {
                for (std::size_t index = 0; index < entries.size(); ++index)
                {
                    const auto label = CatalogSettingLabel(*entries[index]);
                    if (ImGuiMCP::Selectable(label.c_str(), a_state.catalogSetting == static_cast<int>(index)))
                        a_state.catalogSetting = static_cast<int>(index);
                }
                ImGuiMCP::EndCombo();
            }
            const auto* selected = entries[static_cast<std::size_t>(a_state.catalogSetting)];
            ImGuiMCP::TextDisabled("%s", selected->path.c_str());

            const auto filteredTarget = a_state.filtered &&
                                        SliderSettingCatalog::IsFilteredOperation(selected->filterOperation);
            const auto baseLightTarget = selected->path.starts_with("pointLights.");
            if (baseLightTarget) a_state.pendingIgnoreLink = false;
            const auto directInteriorLinkTarget =
                !a_state.filtered && IsInteriorLinkableSliderSetting(selected->path);
            const auto scaleLabel = SKSEMenuSettings::Label("sliderCreatorScale", "Scale");
            const auto ignoreLinkLabel = SKSEMenuSettings::Label("sliderCreatorIgnoreLink", "Ignore Link");
            if (filteredTarget)
            {
                ImGuiMCP::InputFloat((scaleLabel + "##Pending" + a_id).c_str(), &a_state.pendingScale, 0.1f, 1.0f, "%.2f");
            }
            if ((filteredTarget && !baseLightTarget) || directInteriorLinkTarget)
            {
                ImGuiMCP::Checkbox((ignoreLinkLabel + "##Pending" + a_id).c_str(), &a_state.pendingIgnoreLink);
            }
            if (ImGuiMCP::Button((SKSEMenuSettings::Label("addSliderSetting", "Add Setting") + "##" + a_id).c_str()))
            {
                if (selected->aggregate)
                {
                    for (const auto& candidate : SliderSettingCatalog::Entries())
                        if (candidate.domain == domain && candidate.group == selected->group &&
                            Config::IEquals(candidate.target, selected->target) && !candidate.hue.empty())
                            AddCreatorSetting(a_state, candidate.path);
                }
                else
                {
                    AddCreatorSetting(a_state, selected->path);
                }
                if (!SliderSettingCatalog::IsFilteredOperation(selected->filterOperation)) a_state.filtered = false;
            }
            if (!selected->aggregate && !selected->hue.empty() && !selected->target.empty())
            {
                SameActionLine();
                const auto label = SKSEMenuSettings::Label("addAllHues", "Add All Hues") + "##" + a_id;
                if (ImGuiMCP::Button(label.c_str()))
                {
                    for (const auto& candidate : SliderSettingCatalog::Entries())
                        if (candidate.domain == domain && candidate.group == selected->group &&
                            Config::IEquals(candidate.target, selected->target) && !candidate.hue.empty())
                            AddCreatorSetting(a_state, candidate.path);
                }
            }

            if (ImGuiMCP::BeginListBox(("Slider Settings##" + a_id).c_str(), ImGuiMCP::ImVec2(0.0f, 220.0f)))
            {
                if (a_state.settings.empty()) DrawDisplayText("sliderCreatorNoSettings", true);
                for (std::size_t index = 0; index < a_state.settings.size();)
                {
                    auto& target = a_state.settings[index];
                    ImGuiMCP::TextUnformatted(target.setting.c_str());
                    const auto directInteriorLinkSetting =
                        !a_state.filtered && IsInteriorLinkableSliderSetting(target.setting);
                    if (a_state.filtered)
                    {
                        auto scale = static_cast<float>(target.scale);
                        ImGuiMCP::SetNextItemWidth(180.0f);
                        if (ImGuiMCP::InputFloat(
                                (scaleLabel + "##" + a_id + std::to_string(index)).c_str(),
                                &scale,
                                0.1f,
                                1.0f,
                                "%.2f"))
                            target.scale = scale;
                    }
                    const auto baseLightSetting = target.setting.starts_with("pointLights.");
                    if ((a_state.filtered && !baseLightSetting) || directInteriorLinkSetting)
                    {
                        if (a_state.filtered) SameActionLine();
                        ImGuiMCP::Checkbox(
                            (ignoreLinkLabel + "##" + a_id + std::to_string(index)).c_str(),
                            &target.ignoreLink);
                        SameActionLine();
                    }
                    const auto remove = ImGuiMCP::Button(
                        (SKSEMenuSettings::Label("removeSliderSetting", "Remove") + "##" + a_id + std::to_string(index)).c_str());
                    ImGuiMCP::Separator();
                    if (remove) a_state.settings.erase(a_state.settings.begin() + static_cast<std::ptrdiff_t>(index));
                    else ++index;
                }
                ImGuiMCP::EndListBox();
            }
        }

        SliderCreator::Definition CreatorDefinition(const SliderCreatorState& a_state)
        {
            SliderCreator::Definition result;
            result.label = InputText(a_state.label);
            result.id = MakeSliderID(result.label);
            result.tooltip = InputText(a_state.tooltip);
            const auto filtered = a_state.filtered;
            const auto effectLightingWeatherFilter = CreatorUsesEffectLightingWeatherFilter(a_state);
            const auto weatherFilter = CreatorUsesWeatherFilter(a_state);
            const auto baseLightFilter = CreatorUsesBaseLightFilter(a_state);
            result.link = filtered ? "" : InputText(a_state.link);
            result.localLink = filtered && a_state.domain == SliderCreatorDomain::weather &&
                                       !effectLightingWeatherFilter ?
                                   InputText(a_state.localLink) :
                                   "";
            result.settings = a_state.settings;
            if (baseLightFilter)
                for (auto& setting : result.settings) setting.ignoreLink = false;
            result.filtered = filtered;
            result.filterDomain = weatherFilter ?
                                      SliderCreator::FilterDomain::weather :
                                  baseLightFilter ?
                                      SliderCreator::FilterDomain::baseLight :
                                      SliderCreator::FilterDomain::lightingTemplate;
            if (filtered && weatherFilter && a_state.useHueScales)
            {
                result.hueScales = SliderCreator::HueScales{
                    a_state.hueScales[0],
                    a_state.hueScales[1],
                    a_state.hueScales[2],
                    a_state.hueScales[3],
                    a_state.hueScales[4],
                    a_state.hueScales[5],
                    a_state.hueScales[6],
                };
            }
            result.invert = a_state.invert;
            result.useTimes = filtered && weatherFilter && a_state.useTimes;
            result.times = a_state.times;
            result.include = filtered ? a_state.include : SliderCreator::Filter{};
            result.exclude = filtered ? a_state.exclude : SliderCreator::Filter{};
            if (a_state.useDefault) result.defaultValue = a_state.defaultValue;
            if (a_state.useRange)
            {
                result.minimum = a_state.minimum;
                result.maximum = a_state.maximum;
                result.step = a_state.step;
            }
            if (a_state.useWidth) result.width = a_state.width;
            result.format = InputText(a_state.format);
            return result;
        }

        std::string SliderCreatorStyleSetting(const SliderCreatorState& a_state)
        {
            auto setting = a_state.settings.empty() ?
                               std::string("generic") :
                               a_state.settings.front().setting;
            if (!a_state.filtered) return setting;

            const auto operation = CreatorFilteredOperation(a_state);
            if (CreatorUsesBaseLightFilter(a_state)) return setting;
            if (CreatorUsesEffectLightingWeatherFilter(a_state))
            {
                if (!operation) return setting;
                switch (*operation)
                {
                case SliderSettingCatalog::FilterOperation::brightness:
                    return "fxEffectLighting.brightnessMultiplier";
                case SliderSettingCatalog::FilterOperation::saturation:
                    return "fxEffectLighting.saturationMultiplier";
                case SliderSettingCatalog::FilterOperation::hueShift:
                    return "fxEffectLighting.hueShift";
                default:
                    return setting;
                }
            }

            if (a_state.domain == SliderCreatorDomain::interior)
            {
                return operation && *operation == SliderSettingCatalog::FilterOperation::fogStrength ?
                           "intFogMaxMultiplier" :
                           "intBrightnessMultiplier";
            }

            if (!operation) return setting;
            switch (*operation)
            {
            case SliderSettingCatalog::FilterOperation::brightness:
                return "brightnessMultiplier";
            case SliderSettingCatalog::FilterOperation::saturation:
                return "saturationMultiplier";
            case SliderSettingCatalog::FilterOperation::hueShift:
                return "hueShift";
            case SliderSettingCatalog::FilterOperation::fogStrength:
                return "intFogMaxMultiplier";
            default:
                return setting;
            }
        }

        std::string SliderCreatorAutomaticFormat(const SliderCreatorState& a_state)
        {
            MenuControl control;
            if (a_state.useRange) control.step = a_state.step;
            return ResolveControlSliderDefaults(
                       control,
                       SliderCreatorStyleSetting(a_state))
                .format;
        }

        struct SliderCreatorFormatOption
        {
            std::string_view format;
            std::string_view sample;
        };

        constexpr std::array kSliderCreatorFormatOptions{
            SliderCreatorFormatOption{ "%.0f", "1" },
            SliderCreatorFormatOption{ "%.1f", "0.1" },
            SliderCreatorFormatOption{ "%.2f", "0.01" },
        };

        std::string SliderCreatorFormatSample(const std::string_view a_format)
        {
            const auto option = std::ranges::find(kSliderCreatorFormatOptions, a_format, &SliderCreatorFormatOption::format);
            return option == kSliderCreatorFormatOptions.end() ? std::string(a_format) : std::string(option->sample);
        }

        float SliderCreatorPreviewInitialValue(const SliderCreatorState& a_state)
        {
            const auto operation = CreatorFilteredOperation(a_state);
            const auto filtered = a_state.filtered;
            if (filtered)
            {
                if (a_state.useDefault) return a_state.defaultValue;
                return operation && *operation == SliderSettingCatalog::FilterOperation::hueShift &&
                               InputText(a_state.localLink).empty() ?
                           0.0f :
                           1.0f;
            }
            if (a_state.settings.empty()) return 1.0f;

            auto profile = a_state.profile;
            const auto setting = FindSliderSetting(
                TuningUtil::GetSettings(profile),
                a_state.settings.front().setting);
            if (!setting) return 1.0f;
            if (!InputText(a_state.link).empty() && setting->link)
            {
                return static_cast<float>(ReadLinkable(*setting->link).scale);
            }
            return static_cast<float>(setting->resolved);
        }

        void DrawSliderCreatorFunctionalPreview(
            SliderCreatorState& a_state,
            const std::string& a_id)
        {
            const auto operation = CreatorFilteredOperation(a_state);
            const auto filtered = a_state.filtered;
            const auto firstSetting = a_state.settings.empty() ?
                                          std::string{} :
                                          a_state.settings.front().setting;
            const auto previewKey = std::format(
                "{}:{}:{}:{}:{}:{}",
                filtered,
                static_cast<int>(operation.value_or(SliderSettingCatalog::FilterOperation::none)),
                firstSetting,
                InputText(a_state.link),
                InputText(a_state.localLink),
                filtered && a_state.useDefault ? std::to_string(a_state.defaultValue) : std::string{});
            if (!std::isfinite(a_state.functionalPreviewValue) || a_state.functionalPreviewKey != previewKey)
            {
                a_state.functionalPreviewValue = SliderCreatorPreviewInitialValue(a_state);
                a_state.functionalPreviewKey = previewKey;
            }

            const auto styleSetting = SliderCreatorStyleSetting(a_state);

            MenuControl previewControl;
            previewControl.setting = styleSetting;
            if (a_state.useRange)
            {
                previewControl.min = a_state.minimum;
                previewControl.max = a_state.maximum;
                previewControl.step = a_state.step;
            }
            if (a_state.useWidth) previewControl.width = a_state.width;
            previewControl.format = InputText(a_state.format);
            const auto grouped = !filtered && (a_state.settings.size() > 1 || !InputText(a_state.link).empty());
            const auto defaults = ResolveControlSliderDefaults(
                previewControl,
                styleSetting,
                grouped ? -1.0f : 0.0f);
            auto minimum = std::min(defaults.minimum, defaults.maximum);
            auto maximum = std::max(defaults.minimum, defaults.maximum);
            if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum == maximum)
            {
                const auto fallback = FallbackSliderRange(styleSetting);
                minimum = fallback.minimum;
                maximum = fallback.maximum;
            }
            const auto* format = IsSafeSliderFormat(defaults.format) ? defaults.format.c_str() : "%.2f";
            const auto inverted = a_state.invert && (filtered || InputText(a_state.link).empty());
            auto value = inverted ? -a_state.functionalPreviewValue : a_state.functionalPreviewValue;
            const auto neutralValue = inverted || !InputText(a_state.link).empty() ?
                                          std::nullopt :
                                          SliderNeutralValue(firstSetting);
            auto label = InputText(a_state.label);
            if (label.empty()) label = DisplayText("sliderCreatorNewSlider");

            constexpr auto flags = ImGuiMCP::ImGuiChildFlags_Border |
                                   ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                                   ImGuiMCP::ImGuiChildFlags_AutoResizeY;
            const auto padding = SKSEMenuSettings::GetBoxPadding();
            const auto customPadding = padding[0] > 0.0f || padding[1] > 0.0f;
            if (customPadding)
            {
                ImGuiMCP::PushStyleVar(
                    ImGuiMCP::ImGuiStyleVar_WindowPadding,
                    ImGuiMCP::ImVec2(padding[0], padding[1]));
            }
            const auto visible = ImGuiMCP::BeginChild(
                ("FunctionalPreviewBox##" + a_id).c_str(),
                ImGuiMCP::ImVec2(0.0f, 0.0f),
                flags);
            if (visible)
            {
                DrawHeader(SKSEMenuSettings::Label("sliderCreatorFunctionalPreview", "Functional Preview"));
                ImGuiMCP::BeginGroup();
                if (DrawSliderWithInput(
                        label + "##FunctionalPreview" + a_id,
                        value,
                        minimum,
                        maximum,
                        defaults.step,
                        format,
                        defaults.width,
                        SliderInputRange::standard,
                        neutralValue))
                {
                    a_state.functionalPreviewValue = inverted ? -value : value;
                }
                ImGuiMCP::EndGroup();
                DrawItemTooltip(InputText(a_state.tooltip));
            }
            ImGuiMCP::EndChild();
            if (customPadding) ImGuiMCP::PopStyleVar();
            ImGuiMCP::Spacing();
        }

        void DrawSliderCreatorAdvancedSettings(
            SliderCreatorState& a_state,
            const std::string& a_id,
            const std::optional<SliderSettingCatalog::FilterOperation> a_operation)
        {
            const auto effectLightingWeatherFilter = CreatorUsesEffectLightingWeatherFilter(a_state);
            const auto filteredWeatherFeatures = CreatorUsesWeatherFilter(a_state) &&
                                                 a_state.filtered &&
                                                 a_operation.has_value();
            const auto supportsLocalLink = filteredWeatherFeatures &&
                                           a_state.domain == SliderCreatorDomain::weather &&
                                           !effectLightingWeatherFilter;
            const auto supportsHueScales = filteredWeatherFeatures &&
                                           *a_operation == SliderSettingCatalog::FilterOperation::saturation;
            if (!supportsLocalLink) a_state.localLink.fill('\0');
            if (!supportsHueScales) a_state.useHueScales = false;

            const auto advancedLabel = SKSEMenuSettings::Label(
                "sliderCreatorAdvancedSettings",
                "Advanced Settings");
            if (!ImGuiMCP::CollapsingHeader((advancedLabel + "##" + a_id).c_str())) return;

            if (!a_state.filtered)
            {
                const auto linkLabel = SKSEMenuSettings::Label("sliderCreatorLink", "Link");
                const auto linkHint = SKSEMenuSettings::Label(
                    "sliderCreatorLinkHint",
                    "Optional direct grouped link source");
                ImGuiMCP::SetNextItemWidth(260.0f);
                ImGuiMCP::InputTextWithHint(
                    (linkLabel + "##" + a_id).c_str(),
                    linkHint.c_str(),
                    a_state.link.data(),
                    a_state.link.size());
            }
            if (a_state.filtered || InputText(a_state.link).empty())
            {
                const auto invertLabel = SKSEMenuSettings::Label("sliderCreatorInvert", "Invert Slider");
                ImGuiMCP::Checkbox((invertLabel + "##" + a_id).c_str(), &a_state.invert);
            }

            if (filteredWeatherFeatures)
            {
                if (supportsLocalLink)
                {
                    auto localLinkPreview = InputText(a_state.localLink);
                    if (localLinkPreview.empty())
                        localLinkPreview = SKSEMenuSettings::Label("sliderCreatorNoLocalLink", "None");
                    else if (const auto source = std::ranges::find_if(
                                 SliderSettingCatalog::Entries(),
                                 [&](const auto& a_entry)
                                 {
                                     return a_entry.domain == SliderSettingCatalog::Domain::weather &&
                                            a_entry.filterOperation == *a_operation &&
                                            Config::IEquals(a_entry.target, localLinkPreview);
                                 });
                        source != SliderSettingCatalog::Entries().end())
                    {
                        const auto separator = source->label.find(" / ");
                        localLinkPreview = CatalogLabelPart(
                            separator == std::string::npos ? source->label : source->label.substr(0, separator));
                    }
                    const auto localLinkLabel = SKSEMenuSettings::Label("sliderCreatorLocalLink", "Local Link");
                    if (ImGuiMCP::BeginCombo((localLinkLabel + "##" + a_id).c_str(), localLinkPreview.c_str()))
                    {
                        const auto noneLabel = SKSEMenuSettings::Label("sliderCreatorNoLocalLink", "None");
                        if (ImGuiMCP::Selectable(noneLabel.c_str(), InputText(a_state.localLink).empty()))
                            a_state.localLink.fill('\0');
                        std::unordered_set<std::string> targets;
                        for (const auto& entry : SliderSettingCatalog::Entries())
                        {
                            if (entry.domain != SliderSettingCatalog::Domain::weather ||
                                entry.filterOperation != *a_operation || entry.target.empty() ||
                                !targets.insert(Lowercase(entry.target)).second)
                                continue;
                            const auto separator = entry.label.find(" / ");
                            const auto sourceLabel = separator == std::string::npos ? entry.label : entry.label.substr(0, separator);
                            const auto localizedSource = CatalogLabelPart(sourceLabel);
                            if (ImGuiMCP::Selectable(
                                    localizedSource.c_str(),
                                    Config::IEquals(InputText(a_state.localLink), entry.target)))
                                SetInputText(a_state.localLink, entry.target);
                        }
                        ImGuiMCP::EndCombo();
                    }
                }

                if (supportsHueScales)
                {
                    const auto hueScaleLabel = SKSEMenuSettings::Label(
                        "sliderCreatorUniqueHueScales",
                        "Unique Saturation Scales");
                    ImGuiMCP::Checkbox((hueScaleLabel + "##" + a_id).c_str(), &a_state.useHueScales);
                    if (a_state.useHueScales)
                    {
                        static constexpr std::array hueLabels{ "Red", "Orange", "Yellow", "Green", "Teal", "Blue", "Magenta" };
                        for (std::size_t index = 0; index < hueLabels.size(); ++index)
                        {
                            const auto label = SKSEMenuSettings::SettingHueLabel(Lowercase(hueLabels[index]), hueLabels[index]);
                            ImGuiMCP::InputFloat(
                                (label + "##HueScale" + a_id).c_str(),
                                &a_state.hueScales[index],
                                0.1f,
                                1.0f,
                                "%.2f");
                        }
                    }
                }

            }

            if (a_state.filtered)
            {
                const auto customDefaultLabel = SKSEMenuSettings::Label("sliderCreatorCustomDefault", "Custom Default");
                ImGuiMCP::Checkbox((customDefaultLabel + "##" + a_id).c_str(), &a_state.useDefault);
                if (a_state.useDefault)
                {
                    const auto defaultLabel = SKSEMenuSettings::Label("sliderCreatorDefault", "Default");
                    ImGuiMCP::InputFloat((defaultLabel + "##" + a_id).c_str(), &a_state.defaultValue, 0.1f, 1.0f, "%.3f");
                }
            }

            const auto customRangeLabel = SKSEMenuSettings::Label("sliderCreatorCustomRange", "Custom Range");
            ImGuiMCP::Checkbox((customRangeLabel + "##" + a_id).c_str(), &a_state.useRange);
            if (a_state.useRange)
            {
                const auto minimumLabel = SKSEMenuSettings::Label("sliderCreatorMinimum", "Minimum");
                const auto maximumLabel = SKSEMenuSettings::Label("sliderCreatorMaximum", "Maximum");
                const auto stepLabel = SKSEMenuSettings::Label("sliderCreatorStep", "Step");
                ImGuiMCP::InputFloat((minimumLabel + "##" + a_id).c_str(), &a_state.minimum, 0.1f, 1.0f, "%.3f");
                ImGuiMCP::InputFloat((maximumLabel + "##" + a_id).c_str(), &a_state.maximum, 0.1f, 1.0f, "%.3f");
                ImGuiMCP::InputFloat((stepLabel + "##" + a_id).c_str(), &a_state.step, 0.1f, 1.0f, "%.3f");
            }
            const auto customWidthLabel = SKSEMenuSettings::Label("sliderCreatorCustomWidth", "Custom Width");
            ImGuiMCP::Checkbox((customWidthLabel + "##" + a_id).c_str(), &a_state.useWidth);
            if (a_state.useWidth)
            {
                const auto widthLabel = SKSEMenuSettings::Label("sliderCreatorWidth", "Width");
                ImGuiMCP::InputFloat((widthLabel + "##" + a_id).c_str(), &a_state.width, 10.0f, 50.0f, "%.1f");
            }

            const auto automaticFormat = SliderCreatorAutomaticFormat(a_state);
            const auto automaticLabel = std::format(
                "{} ({})",
                SKSEMenuSettings::Label("sliderCreatorAutomatic", "Automatic"),
                SliderCreatorFormatSample(automaticFormat));
            const auto formatLabel = SKSEMenuSettings::Label("sliderCreatorFormat", "Format");
            const auto selectedFormat = InputText(a_state.format);
            const auto formatPreview = selectedFormat.empty() ?
                                           automaticLabel :
                                           SliderCreatorFormatSample(selectedFormat);
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::BeginCombo(
                (formatLabel + "##" + a_id).c_str(),
                formatPreview.c_str()))
            {
                const auto automaticSelected = selectedFormat.empty();
                if (ImGuiMCP::Selectable(automaticLabel.c_str(), automaticSelected))
                    SetInputText(a_state.format, "");
                if (automaticSelected) ImGuiMCP::SetItemDefaultFocus();

                for (const auto& option : kSliderCreatorFormatOptions)
                {
                    const auto selected = selectedFormat == option.format;
                    if (ImGuiMCP::Selectable(option.sample.data(), selected))
                        SetInputText(a_state.format, option.format);
                    if (selected) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndCombo();
            }
        }

        void DrawSliderCreator(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const SliderCreatorDomain a_domain)
        {
            const auto stateKey = a_menu.profile + ":" + a_control.type + ":" +
                                  (a_control.id.empty() ? "sliderCreator" : a_control.id);
            auto& state = sliderCreatorStates[stateKey];
            if (!state.initialized ||
                !Config::IEquals(
                    state.profile,
                    a_menu.profile))
            {
                std::string error;
                const auto pages = SliderCreator::Load(
                    TuningUtil::ProfileDirectory(a_menu.profile) / kMenuDefinitionFileName,
                    error);
                const auto preferredTitle = a_domain == SliderCreatorDomain::weather ? "Weather Slider" : "Interior Slider";
                const auto preferred = std::ranges::find_if(pages, [&](const auto& a_page)
                    { return Config::IEquals(a_page.title, preferredTitle); });
                const auto customSliders = std::ranges::find_if(pages, [](const auto& a_page)
                    { return Config::IEquals(a_page.title, "Custom Sliders"); });
                const auto initialPage = preferred != pages.end() ? preferred : customSliders;
                ResetSliderCreator(
                    state,
                    a_menu.profile,
                    initialPage != pages.end() ?
                        static_cast<std::size_t>(std::distance(pages.begin(), initialPage)) :
                        0,
                    a_domain);
            }

            const auto menuPath = TuningUtil::ProfileDirectory(a_menu.profile) / kMenuDefinitionFileName;
            std::string loadError;
            auto pages = SliderCreator::Load(menuPath, loadError);
            if (pages.empty())
            {
                DrawDisplayText("sliderCreatorMenuLoadFailure", false, { { "reason", SliderCreatorErrorText(loadError) } });
                return;
            }
            state.pageIndex = std::min(state.pageIndex, pages.size() - 1);
            if (state.loadedPageIndex &&
                state.loadedControlIndex)
            {
                const auto valid =
                    *state.loadedPageIndex < pages.size() &&
                    std::ranges::any_of(
                        pages[*state.loadedPageIndex].sliders,
                        [&](const auto& a_slider)
                        {
                            return a_slider.controlIndex ==
                                       *state.loadedControlIndex &&
                                   Config::IEquals(
                                       a_slider.definition.id,
                                       state.loadedSliderID);
                        });
                if (!valid)
                {
                    state.loadedPageIndex.reset();
                    state.loadedControlIndex.reset();
                    state.loadedSliderID.clear();
                }
            }
            else
            {
                state.loadedPageIndex.reset();
                state.loadedControlIndex.reset();
                state.loadedSliderID.clear();
            }

            DrawHeader(
                SKSEMenuSettings::Label(
                    "sliderCreatorSliderSection",
                    "Slider"));
            auto existingPreview = DisplayText("sliderCreatorNewSlider");
            if (state.loadedPageIndex &&
                state.loadedControlIndex)
            {
                const auto existing = std::ranges::find(
                    pages[*state.loadedPageIndex].sliders,
                    *state.loadedControlIndex,
                    &SliderCreator::ExistingSlider::controlIndex);
                if (existing !=
                    pages[*state.loadedPageIndex].sliders.end())
                {
                    const auto name =
                        existing->definition.label.empty() ?
                            existing->definition.id :
                            existing->definition.label;
                    existingPreview = std::format(
                        "{} ({})",
                        name,
                        pages[*state.loadedPageIndex].title);
                }
            }
            const auto loadExistingLabel =
                SKSEMenuSettings::Label(
                    "sliderCreatorLoadExisting",
                    "Load Existing Slider");
            if (ImGuiMCP::BeginCombo(
                    (loadExistingLabel + "##" + stateKey).c_str(),
                    existingPreview.c_str(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                if (ImGuiMCP::Selectable(
                        DisplayText("sliderCreatorNewSlider").c_str(),
                        !state.loadedPageIndex ||
                            !state.loadedControlIndex))
                {
                    ResetSliderCreator(
                        state,
                        state.profile,
                        state.pageIndex,
                        state.domain);
                }
                for (std::size_t pageIndex = 0;
                     pageIndex < pages.size();
                     ++pageIndex)
                {
                    for (const auto& slider :
                         pages[pageIndex].sliders)
                    {
                        const auto name =
                            slider.definition.label.empty() ?
                                slider.definition.id :
                                slider.definition.label;
                        const auto visibleLabel = std::format(
                            "{} ({})",
                            name,
                            pages[pageIndex].title);
                        const auto label = visibleLabel +
                                           "##ExistingSlider" +
                                           stateKey +
                                           std::to_string(pageIndex) +
                                           ":" +
                                           std::to_string(
                                               slider.controlIndex);
                        const auto selected =
                            state.loadedPageIndex == pageIndex &&
                            state.loadedControlIndex ==
                                slider.controlIndex;
                        if (ImGuiMCP::Selectable(
                                label.c_str(),
                                selected))
                        {
                            LoadSliderCreatorDefinition(
                                state,
                                pageIndex,
                                slider);
                        }
                    }
                }
                ImGuiMCP::EndCombo();
            }

            ImGuiMCP::SetNextItemWidth(320.0f);
            ImGuiMCP::InputTextWithHint(("Name##" + stateKey).c_str(), "Slider name", state.label.data(), state.label.size());
            ImGuiMCP::SetNextItemWidth(420.0f);
            ImGuiMCP::InputTextWithHint(("Tooltip##" + stateKey).c_str(), "Optional hover description", state.tooltip.data(), state.tooltip.size());
            const auto filteredOperation = CreatorFilteredOperation(state);
            const auto supportsFiltering =
                (state.settings.empty() ||
                 filteredOperation.has_value());
            if (!supportsFiltering) state.filtered = false;
            if (supportsFiltering)
            {
                const auto filterLabel = CreatorUsesWeatherFilter(state) ?
                                             SKSEMenuSettings::Label(
                                                 "sliderCreatorFilteredWeather",
                                                 "Filtered Weather Slider") :
                                         CreatorUsesBaseLightFilter(state) ?
                                             SKSEMenuSettings::Label(
                                                 "sliderCreatorFilteredBaseLight",
                                                 "Filtered Base Light Slider") :
                                             SKSEMenuSettings::Label(
                                                 "sliderCreatorFilteredLightingTemplate",
                                                 "Filtered Lighting Template Slider");
                ImGuiMCP::Checkbox((filterLabel + "##" + stateKey).c_str(), &state.filtered);
            }

            DrawHeader(
                SKSEMenuSettings::Label(
                    "sliderCreatorSettingsSection",
                    "Settings"));
            DrawSliderCreatorSettings(state, stateKey);

            const auto currentOperation = CreatorFilteredOperation(state);
            if (!state.settings.empty() && !currentOperation) state.filtered = false;
            const auto filteredFeatures =
                state.filtered &&
                currentOperation.has_value();

            DrawSliderCreatorAdvancedSettings(state, stateKey, currentOperation);

            if (filteredFeatures && CreatorUsesWeatherFilter(state))
            {
                DrawHeader("Time Filter");
                ImGuiMCP::Checkbox(("Enable Time Filter##" + stateKey).c_str(), &state.useTimes);
                if (state.useTimes)
                {
                    static constexpr std::array timeLabels{ "Sunrise", "Day", "Sunset", "Night" };
                    for (std::size_t index = 0; index < timeLabels.size(); ++index)
                    {
                        if (index > 0) SameActionLine();
                        ImGuiMCP::Checkbox(
                            (std::string(timeLabels[index]) + "##" + stateKey).c_str(),
                            &state.times[index]);
                    }
                }

                DrawHeader("Weather Filter");
                const auto& weatherEntries = GetSliderCreatorWeatherEntries(
                    state.profile,
                    CreatorUsesEffectLightingWeatherFilter(state));
                auto selected = std::ranges::find(weatherEntries, state.selectedWeather, &WeatherMenuEntry::weather);
                const auto weatherPreview = selected != weatherEntries.end() ? selected->label : DisplayText("selectWeather");
                if (ImGuiMCP::BeginCombo(("Weather##SliderCreator" + stateKey).c_str(), weatherPreview.c_str(), ImGuiMCP::ImGuiComboFlags_HeightLargest))
                {
                    for (const auto& entry : weatherEntries)
                    {
                        const auto label = entry.label + "##SliderCreatorWeather" + stateKey +
                                           std::format("{:08X}", entry.weather->GetFormID());
                        if (ImGuiMCP::Selectable(label.c_str(), entry.weather == state.selectedWeather))
                            state.selectedWeather = entry.weather;
                    }
                    ImGuiMCP::EndCombo();
                }
                const auto weatherKey = WeatherExclusionKey(state.selectedWeather);
                ImGuiMCP::BeginDisabled(weatherKey.empty());
                if (ImGuiMCP::Button((SKSEMenuSettings::Label("addIncludedWeather", "Add to Include") + "##" + stateKey).c_str()))
                {
                    AddUniqueString(state.include.formIDs, weatherKey);
                    std::erase_if(state.exclude.formIDs, [&](const auto& a_value)
                        { return Config::IEquals(a_value, weatherKey); });
                }
                SameActionLine();
                if (ImGuiMCP::Button((SKSEMenuSettings::Label("addExcludedWeather", "Add to Exclude") + "##" + stateKey).c_str()))
                {
                    AddUniqueString(state.exclude.formIDs, weatherKey);
                    std::erase_if(state.include.formIDs, [&](const auto& a_value)
                        { return Config::IEquals(a_value, weatherKey); });
                }
                ImGuiMCP::EndDisabled();

                DrawHeader("Included Weathers");
                DrawCreatorWeatherList(state.include.formIDs, "Included Weathers##" + stateKey);
                DrawCreatorContainsList(
                    state.include.contains,
                    state.includeContainsInput,
                    state.includeContainsSelection,
                    "Include" + stateKey);
                DrawHeader("Excluded Weathers");
                DrawCreatorWeatherList(state.exclude.formIDs, "Excluded Weathers##" + stateKey);
                DrawCreatorContainsList(
                    state.exclude.contains,
                    state.excludeContainsInput,
                    state.excludeContainsSelection,
                    "Exclude" + stateKey);
            }
            else if (filteredFeatures && CreatorUsesBaseLightFilter(state))
            {
                DrawHeader(SKSEMenuSettings::Label("baseLightFilter", "Base Light Filter"));
                const auto& entries = GetBaseLightMenuEntries();
                const auto selected = std::ranges::find(
                    entries,
                    state.selectedBaseLight,
                    [](const RecordMenuEntry& a_entry) { return a_entry.form; });
                if (selected == entries.end()) state.selectedBaseLight = nullptr;
                const auto preview = selected != entries.end() ? selected->label : DisplayText("selectRecord");
                const auto selectorLabel = SKSEMenuSettings::Label("baseLight", "Base Light") +
                                           "##SliderCreator" + stateKey;
                if (ImGuiMCP::BeginCombo(
                        selectorLabel.c_str(),
                        preview.c_str(),
                        ImGuiMCP::ImGuiComboFlags_HeightLargest))
                {
                    for (const auto& entry : entries)
                    {
                        const auto label = entry.label + "##SliderCreatorBaseLight" + stateKey +
                                           std::format("{:08X}", entry.form->GetFormID());
                        if (ImGuiMCP::Selectable(label.c_str(), entry.form == state.selectedBaseLight))
                            state.selectedBaseLight = static_cast<RE::TESObjectLIGH*>(entry.form);
                    }
                    ImGuiMCP::EndCombo();
                }

                const auto lightKey = RecordFilter::FormKey(state.selectedBaseLight);
                ImGuiMCP::BeginDisabled(lightKey.empty());
                if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToIncluded", "Add to Included") +
                                      "##SliderCreatorBaseLight" + stateKey)
                            .c_str()))
                {
                    AddUniqueString(state.include.formIDs, lightKey);
                    std::erase_if(state.exclude.formIDs, [&](const auto& a_value)
                        { return Config::IEquals(a_value, lightKey); });
                }
                SameActionLine();
                if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToExcluded", "Add to Excluded") +
                                      "##SliderCreatorBaseLight" + stateKey)
                            .c_str()))
                {
                    AddUniqueString(state.exclude.formIDs, lightKey);
                    std::erase_if(state.include.formIDs, [&](const auto& a_value)
                        { return Config::IEquals(a_value, lightKey); });
                }
                ImGuiMCP::EndDisabled();

                DrawHeader(SKSEMenuSettings::Label("includedRecords", "Included Records"));
                DrawCreatorRecordList(
                    state.include.formIDs,
                    "Included Base Lights##" + stateKey,
                    RecordFilterKind::baseLight);
                DrawCreatorContainsList(
                    state.include.contains,
                    state.includeContainsInput,
                    state.includeContainsSelection,
                    "BaseLightInclude" + stateKey);
                DrawHeader(SKSEMenuSettings::Label("excludedRecords", "Excluded Records"));
                DrawCreatorRecordList(
                    state.exclude.formIDs,
                    "Excluded Base Lights##" + stateKey,
                    RecordFilterKind::baseLight);
                DrawCreatorContainsList(
                    state.exclude.contains,
                    state.excludeContainsInput,
                    state.excludeContainsSelection,
                    "BaseLightExclude" + stateKey);
            }
            else if (filteredFeatures)
            {
                DrawHeader(SKSEMenuSettings::Label("lightingTemplateFilter", "Lighting Template Filter"));
                const auto& entries = GetLightingTemplateMenuEntries();
                const auto selected = std::ranges::find(
                    entries,
                    state.selectedLightingTemplate,
                    [](const RecordMenuEntry& a_entry) { return a_entry.form; });
                if (selected == entries.end()) state.selectedLightingTemplate = nullptr;
                const auto preview = selected != entries.end() ?
                                         selected->label :
                                         DisplayText("selectRecord");
                const auto selectorLabel =
                    SKSEMenuSettings::Label("lightingTemplate", "Lighting Template") +
                    "##SliderCreator" + stateKey;
                if (ImGuiMCP::BeginCombo(
                        selectorLabel.c_str(),
                        preview.c_str(),
                        ImGuiMCP::ImGuiComboFlags_HeightLargest))
                {
                    for (const auto& entry : entries)
                    {
                        const auto label = entry.label + "##SliderCreatorLightingTemplate" + stateKey +
                                           std::format("{:08X}", entry.form->GetFormID());
                        if (ImGuiMCP::Selectable(label.c_str(), entry.form == state.selectedLightingTemplate))
                        {
                            state.selectedLightingTemplate = static_cast<RE::BGSLightingTemplate*>(entry.form);
                        }
                    }
                    ImGuiMCP::EndCombo();
                }

                const auto templateKey = RecordFilter::FormKey(state.selectedLightingTemplate);
                ImGuiMCP::BeginDisabled(templateKey.empty());
                if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToIncluded", "Add to Included") +
                                      "##SliderCreatorLightingTemplate" + stateKey)
                            .c_str()))
                {
                    AddUniqueString(state.include.formIDs, templateKey);
                    std::erase_if(state.exclude.formIDs, [&](const auto& a_value)
                        { return Config::IEquals(a_value, templateKey); });
                }
                SameActionLine();
                if (ImGuiMCP::Button((SKSEMenuSettings::Label("addToExcluded", "Add to Excluded") +
                                      "##SliderCreatorLightingTemplate" + stateKey)
                            .c_str()))
                {
                    AddUniqueString(state.exclude.formIDs, templateKey);
                    std::erase_if(state.include.formIDs, [&](const auto& a_value)
                        { return Config::IEquals(a_value, templateKey); });
                }
                ImGuiMCP::EndDisabled();

                DrawHeader(SKSEMenuSettings::Label("includedRecords", "Included Records"));
                DrawCreatorRecordList(
                    state.include.formIDs,
                    "Included Lighting Templates##" + stateKey,
                    RecordFilterKind::lightingTemplate);
                DrawCreatorContainsList(
                    state.include.contains,
                    state.includeContainsInput,
                    state.includeContainsSelection,
                    "LightingTemplateInclude" + stateKey);
                DrawHeader(SKSEMenuSettings::Label("excludedRecords", "Excluded Records"));
                DrawCreatorRecordList(
                    state.exclude.formIDs,
                    "Excluded Lighting Templates##" + stateKey,
                    RecordFilterKind::lightingTemplate);
                DrawCreatorContainsList(
                    state.exclude.contains,
                    state.excludeContainsInput,
                    state.excludeContainsSelection,
                    "LightingTemplateExclude" + stateKey);
            }

            DrawSliderCreatorFunctionalPreview(state, stateKey);

            DrawHeader(
                SKSEMenuSettings::Label(
                    "sliderCreatorAddSliderSection",
                    "Add Slider"));
            const auto editing =
                state.loadedPageIndex &&
                state.loadedControlIndex;
            const auto actionPageIndex =
                editing ?
                    *state.loadedPageIndex :
                    state.pageIndex;
            ImGuiMCP::BeginDisabled(editing);
            const auto pageSelectionLabel =
                SKSEMenuSettings::Label(
                    "sliderCreatorPageSelection",
                    "Page Selection");
            if (ImGuiMCP::BeginCombo(
                    (pageSelectionLabel + "##" + stateKey).c_str(),
                    pages[actionPageIndex].title.c_str(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                for (std::size_t index = 0;
                     index < pages.size();
                     ++index)
                {
                    if (ImGuiMCP::Selectable(
                            pages[index].title.c_str(),
                            index == state.pageIndex))
                    {
                        state.pageIndex = index;
                    }
                }
                ImGuiMCP::EndCombo();
            }
            ImGuiMCP::EndDisabled();

            const ButtonColorStyle saveColor(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
            const auto saveLabel = SKSEMenuSettings::Label(
                editing ? "updateSlider" : "addSlider",
                editing ? "Update Slider" : "Add Slider");
            if (ImGuiMCP::Button((saveLabel + "##" + stateKey).c_str()))
            {
                auto definition = CreatorDefinition(state);
                std::string error;
                if (SliderCreator::Save(
                        menuPath,
                        actionPageIndex,
                        editing ?
                            state.loadedControlIndex :
                            std::nullopt,
                        definition,
                        error))
                {
                    const auto reloaded = SliderCreator::Load(menuPath, error);
                    if (actionPageIndex < reloaded.size())
                    {
                        const auto slider = std::ranges::find_if(reloaded[actionPageIndex].sliders, [&](const auto& a_slider)
                            { return Config::IEquals(a_slider.definition.id, definition.id); });
                        if (slider != reloaded[actionPageIndex].sliders.end())
                        {
                            state.loadedPageIndex =
                                actionPageIndex;
                            state.loadedControlIndex =
                                slider->controlIndex;
                            state.loadedSliderID =
                                definition.id;
                        }
                    }
                    loadedDefinitionFiles.clear();
                    nextDefinitionCheck = {};
                    statusMessage = StatusText(
                        "sliderSaved",
                        {
                            { "slider", definition.label },
                            { "profile", state.profile },
                            { "page", pages[actionPageIndex].title },
                        });
                    pendingMenuReloadStatus = statusMessage.text;
                }
                else
                {
                    logger::warn("[Tuning Menu] slider save failed | slider={} | path={} | {}", definition.id, menuPath.string(), error);
                    statusMessage = StatusText("sliderSaveFailure", { { "reason", SliderCreatorErrorText(error) } });
                }
            }
        }

        void RefreshProfileMenuState(
            const MenuDefinition& a_menu,
            const std::span<const std::string> a_scopes = {})
        {
            auto profile = a_menu.profile;
            if (!HasWeatherMenuControls(a_menu))
            {
                return;
            }
            weatherMenuEntries.clear();
            sliderCreatorWeatherEntries.clear();
            if (a_scopes.empty() ||
                std::ranges::contains(a_scopes, "EnableProfile") ||
                std::ranges::contains(a_scopes, "profile"))
            {
                activeWeatherLockProfile.clear();
                ActivateWeatherLockPreference(a_menu);
            }
        }

        void SaveAllSettings(const MenuDefinition& a_menu)
        {
            auto profile = a_menu.profile;
            const auto saved = TuningUtil::SaveSettings(profile);
            statusMessage = saved ?
                                StatusText("saveAllSuccess", { { "profile", a_menu.title } }) :
                                StatusText("saveAllFailure");
        }

        std::vector<std::string> PageResetScopes(
            const MenuDefinition& a_menu,
            const std::span<const MenuControl> a_controls)
        {
            std::vector<std::string> scopes;
            const auto addScope = [&](const std::string_view a_scope)
            {
                if (!a_scope.empty() && !std::ranges::contains(scopes, a_scope))
                {
                    scopes.emplace_back(a_scope);
                }
            };

            for (const auto& control : a_controls)
            {
                if (control.type == "slider")
                {
                    if (!control.id.empty() && TuningUtil::FindFilteredWeatherRule(a_menu.profile, control.id))
                    {
                        addScope("filteredWeatherAdjustments." + control.id);
                    }
                    else if (!control.id.empty() &&
                             TuningUtil::FindFilteredLightingTemplateRule(a_menu.profile, control.id))
                    {
                        addScope("filteredLightingTemplateAdjustments." + control.id);
                    }
                    else if (!control.id.empty() &&
                             TuningUtil::FindFilteredBaseLightRule(a_menu.profile, control.id))
                    {
                        addScope("filteredBaseLightAdjustments." + control.id);
                    }
                    else if (!control.settings.empty())
                    {
                        for (const auto& setting : control.settings) addScope(SliderTargetPath(setting));
                    }
                    else
                    {
                        addScope(control.setting);
                    }
                }
                else if (control.type == "settings")
                {
                    addScope(control.setting);
                    for (const auto& rule : TuningUtil::GetFilteredWeatherRules(a_menu.profile))
                    {
                        if (Config::IEquals(rule.controlID, control.id))
                        {
                            addScope("filteredWeatherAdjustments." + rule.id);
                        }
                    }
                }
                else if (control.type == "links")
                {
                    if (Config::IEquals(control.setting, "weather") || Config::IEquals(control.setting, "interior"))
                        addScope("links." + control.setting);
                }
                else if (control.type == "weatherSetup")
                {
                    static constexpr std::array scopes{
                        "compressionAnchor",
                        "weatherInclusions",
                        "weatherExclusions",
                        "pluginInclusions",
                        "pluginExclusions",
                        "hueRanges",
                        "hueScales",
                    };
                    for (const auto scope : scopes) addScope(scope);
                }
                else if (control.type == "ambientWithinGauge")
                {
                    addScope("dynamicAmbientWithin");
                }
                else if (control.type == "ambientBetweenGauge")
                {
                    addScope("dynamicAmbientBetween");
                }
                else if (control.type == "sunlightWithinGauge")
                {
                    addScope("dynamicSunlightWithin");
                }
                else if (control.type == "sunlightBetweenGauge")
                {
                    addScope("dynamicSunlightBetween");
                }
                else if (control.type == "interiorSetup")
                {
                    static constexpr std::array scopes{
                        "intHueRanges",
                        "intAmbientHueScales",
                        "lightingTemplateInclusions",
                        "lightingTemplateExclusions",
                        "lightingTemplatePluginInclusions",
                        "lightingTemplatePluginExclusions",
                        "effectPointLightInclusions",
                        "effectPointLightExclusions",
                        "effectLightingPluginInclusions",
                        "effectLightingPluginExclusions",
                    };
                    for (const auto scope : scopes) addScope(scope);
                }
            }
            return scopes;
        }

        void ResetProfilePage(const MenuDefinition& a_menu, const std::span<const MenuControl> a_controls)
        {
            const auto scopes = PageResetScopes(a_menu, a_controls);
            if (scopes.empty())
            {
                statusMessage = StatusText("pageHasNoSettings");
                return;
            }

            auto profile = a_menu.profile;
            const auto reset = TuningUtil::ResetSettingsToDefault(profile, scopes);
            if (reset)
            {
                RefreshProfileMenuState(a_menu, scopes);
            }
            statusMessage = reset ?
                                StatusText("resetPageSuccess") :
                                StatusText("resetPageFailure");
        }

        void SaveProfilePage(const MenuDefinition& a_menu, const std::span<const MenuControl> a_controls)
        {
            const auto scopes = PageResetScopes(a_menu, a_controls);
            auto profile = a_menu.profile;
            const auto saved = !scopes.empty() && TuningUtil::SavePageSettings(profile, scopes);
            statusMessage = saved ?
                                StatusText("savePageSuccess") :
                                StatusText("savePageFailure");
        }

        void RestoreProfilePage(const MenuDefinition& a_menu, const std::span<const MenuControl> a_controls)
        {
            const auto scopes = PageResetScopes(a_menu, a_controls);
            auto profile = a_menu.profile;
            const auto restored = !scopes.empty() && TuningUtil::RestorePageSettings(profile, scopes);
            if (restored)
            {
                RefreshProfileMenuState(a_menu, scopes);
            }
            statusMessage = restored ?
                                StatusText("restorePageSuccess") :
                                StatusText("restorePageFailure");
        }

        void DrawSaveAll(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            const ButtonColorStyle color(a_control.color ? a_control.color : SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
            const auto label = ControlLabel(a_control, "saveAll", "Save All") + "##" + a_id;
            if (ImGuiMCP::Button(label.c_str()))
            {
                SaveAllSettings(a_menu);
            }
        }

        void DrawRestoreAll(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            const ButtonColorStyle color(a_control.color ? a_control.color : SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::restore));
            const auto label = ControlLabel(a_control, "restoreAll", "Restore All") + "##" + a_id;
            if (ImGuiMCP::Button(label.c_str()))
            {
                auto profile = a_menu.profile;
                const auto restored = TuningUtil::RestoreSettings(profile);
                if (restored)
                {
                    WeatherPatcher::DiscardPresetPreview(profile);
                    profilePriorityInputs[profile] = TuningUtil::GetSettings(profile).profilePriority;
                    RefreshProfileMenuState(a_menu);
                }
                statusMessage = restored ?
                                    StatusText("restoreAllSuccess") :
                                    StatusText("restoreAllFailure");
            }
        }

        void DrawResetAll(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            const ButtonColorStyle color(a_control.color ? a_control.color : SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::reset));
            const auto label = ControlLabel(a_control, "resetAll", "Reset All to Defaults") + "##" + a_id;
            if (ImGuiMCP::Button(label.c_str()))
            {
                auto profile = a_menu.profile;
                const auto reset = TuningUtil::ResetAllSettingsToDefault(profile);
                if (reset)
                {
                    WeatherPatcher::DiscardPresetPreview(profile);
                    profilePriorityInputs[profile] = TuningUtil::GetSettings(profile).profilePriority;
                    RefreshProfileMenuState(a_menu);
                }
                statusMessage = reset ?
                                    StatusText("resetAllSuccess") :
                                    StatusText("resetAllFailure");
            }
        }

        int& ProfilePriorityInput(const std::string& a_profile)
        {
            auto profile = a_profile;
            const auto& settings = TuningUtil::GetSettings(profile);
            return profilePriorityInputs.try_emplace(a_profile, settings.profilePriority).first->second;
        }

        void DrawProfilePriority(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            const auto& profile = a_menu.profile;
            auto& priority = ProfilePriorityInput(profile);
            ImGuiMCP::SetNextItemWidth(180.0f);
            const auto label = (a_control.label.empty() ? "Profile Priority" : a_control.label) + "##" + a_id;
            ImGuiMCP::InputInt(label.c_str(), std::addressof(priority));
        }

        void DrawApplyProfilePriority(
            const MenuDefinition& a_menu,
            const MenuControl& a_control,
            const std::string& a_id)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            const ButtonColorStyle color(a_control.color ? a_control.color : SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
            const auto label = ControlLabel(a_control, "applyPriority", "Apply Priority") + "##" + a_id;
            if (ImGuiMCP::Button(label.c_str()))
            {
                settings.profilePriority = ProfilePriorityInput(profile);
                TuningUtil::ApplySettings();
                const std::vector<std::string> priorityScope{ "profilePriority" };
                const auto saved = TuningUtil::SavePageSettings(profile, priorityScope);
                statusMessage = saved ?
                                    StatusText("prioritySaved") :
                                    StatusText("prioritySaveFailure");
            }
        }

        void DrawProfileActionButtons(const MenuDefinition& a_menu)
        {
            MenuControl control{};
            DrawSaveAll(a_menu, control, a_menu.profile + "ProfilePageSaveAll");
            SameActionLine();
            DrawRestoreAll(a_menu, control, a_menu.profile + "ProfilePageRestoreAll");
            SameActionLine();
            DrawResetAll(a_menu, control, a_menu.profile + "ProfilePageResetAll");
        }

        void DrawEnableProfile(const MenuDefinition& a_menu)
        {
            MenuControl control{};
            const auto enableLabel = SKSEMenuSettings::Label("enableProfile", "Enable Profile") +
                                     "##" + a_menu.profile + "ProfilePage";
            DrawProfileToggle(a_menu, control, enableLabel);
        }

        void DrawProfilePriorityModule(const MenuDefinition& a_menu)
        {
            MenuControl control{};
            DrawProfilePriority(a_menu, control, a_menu.profile + "ProfilePagePriority");
            SameActionLine();
            DrawApplyProfilePriority(a_menu, control, a_menu.profile + "ProfilePageApplyPriority");
        }

        void DrawAdvancedToggle(const MenuDefinition& a_menu)
        {
            auto profile = a_menu.profile;
            auto& settings = TuningUtil::GetSettings(profile);
            auto showAdvanced = settings.ShowAdvanced;
            const auto advancedLabel = SKSEMenuSettings::Label("advanced", "Advanced") +
                                       "##" + a_menu.profile;
            if (!ImGuiMCP::Checkbox(advancedLabel.c_str(), &showAdvanced))
            {
                return;
            }

            const auto previous = settings.ShowAdvanced;
            settings.ShowAdvanced = showAdvanced;
            const std::vector<std::string> scope{ "ShowAdvanced" };
            if (!TuningUtil::SavePageSettings(profile, scope))
            {
                settings.ShowAdvanced = previous;
                statusMessage = StatusText("profileStateSaveFailure");
            }
        }

        bool DrawSettingsEditor(const MenuDefinition& a_menu, const MenuControl& a_control)
        {
            return DrawWeatherSettingsEditor(
                       a_menu,
                       a_control) ||
                   DrawLightingSettingsEditor(
                       a_menu,
                       a_control.setting);
        }

        void DrawUnsupportedSettingsEditor(const MenuControl& a_control)
        {
            if (a_control.setting.empty())
            {
                DrawDisplayText("settingsEditorMissingCategory");
            }
            else
            {
                DrawDisplayText(
                    "unsupportedSettingsCategory",
                    false,
                    { { "setting", a_control.setting } });
            }
        }

        void DrawPageActions(
            const MenuDefinition& a_menu,
            const std::span<const MenuControl> a_modules,
            const std::string_view a_id)
        {
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
                const auto label = SKSEMenuSettings::Label("savePage", "Save Page") +
                                   "##" + a_menu.profile + std::string(a_id);
                if (ImGuiMCP::Button(label.c_str())) SaveProfilePage(a_menu, a_modules);
            }
            SameActionLine();
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::restore));
                const auto label = SKSEMenuSettings::Label("restorePage", "Restore Page") +
                                   "##" + a_menu.profile + std::string(a_id);
                if (ImGuiMCP::Button(label.c_str())) RestoreProfilePage(a_menu, a_modules);
            }
            SameActionLine();
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::reset));
                const auto label = SKSEMenuSettings::Label("resetPage", "Reset to Defaults") +
                                   "##" + a_menu.profile + std::string(a_id);
                if (ImGuiMCP::Button(label.c_str())) ResetProfilePage(a_menu, a_modules);
            }
        }

        void DrawModule(
            const MenuDefinition& a_menu,
            const MenuControl& a_module,
            const std::span<const MenuControl> a_pageModules,
            const std::size_t a_index)
        {
            const ButtonColorStyle buttonColor(ControlButtonColor(a_module));
            const auto drawText = [&](const bool a_separator)
            {
                const auto text = ControlDisplayName(a_module, a_module.label);
                const auto scale = std::isfinite(a_module.fontScale) && a_module.fontScale > 0.0f ?
                                       a_module.fontScale :
                                       1.0f;
                if (a_separator)
                {
                    if (!text.empty()) DrawHeader(text, scale);
                    return;
                }
                if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(scale);
                if (!text.empty()) ImGuiMCP::TextWrapped("%s", text.c_str());
                if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(1.0f);
            };

            if (a_module.type == "text" || a_module.type == "separatorText")
            {
                drawText(a_module.type == "separatorText");
                return;
            }
            if (a_module.type == "separator")
            {
                ImGuiMCP::Separator();
                return;
            }
            if (a_module.type == "spacing")
            {
                ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0.0f, ImGuiMCP::GetFrameHeight()));
                return;
            }
            if (a_module.type == "pageActions")
            {
                DrawPageActions(a_menu, a_pageModules, std::to_string(a_index));
                return;
            }
            if (a_module.type == "profileActions")
            {
                DrawProfileActionButtons(a_menu);
                return;
            }
            if (a_module.type == "enableProfile")
            {
                DrawEnableProfile(a_menu);
                return;
            }
            if (a_module.type == "profilePriority")
            {
                DrawProfilePriorityModule(a_menu);
                return;
            }
            if (a_module.type == "advancedToggle")
            {
                DrawAdvancedToggle(a_menu);
                return;
            }
            if (a_module.type == "presetSave")
            {
                DrawSavePresetSelection(a_menu, a_module, a_menu.profile + "PresetCommit" + std::to_string(a_index));
                return;
            }
            if (a_module.type == "presets")
            {
                DrawPresets(a_menu, a_module);
                return;
            }
            if (a_module.type == "presetCreator")
            {
                DrawPresetCreator(a_menu, a_module);
                return;
            }
            if (a_module.type == "slider")
            {
                const auto fallbackLabel = !a_module.settings.empty() ?
                                               std::string(SliderTargetPath(a_module.settings.front())) :
                                               a_module.setting;
                const auto visibleLabel = ControlDisplayName(
                    a_module,
                    a_module.label.empty() ? fallbackLabel : a_module.label);
                const auto label = visibleLabel + "##" + a_menu.profile + "MenuModule" + std::to_string(a_index);
                const auto& profile = a_menu.profile;
                const auto filteredWeather =
                    !a_module.id.empty() && TuningUtil::FindFilteredWeatherRule(profile, a_module.id);
                const auto filteredLightingTemplate =
                    !a_module.id.empty() && TuningUtil::FindFilteredLightingTemplateRule(profile, a_module.id);
                const auto filteredBaseLight =
                    !a_module.id.empty() && TuningUtil::FindFilteredBaseLightRule(profile, a_module.id);
                const auto drawn = filteredWeather ?
                                       DrawFilteredWeatherSlider(a_menu, a_module, label) :
                                   filteredLightingTemplate ?
                                       DrawFilteredLightingTemplateSlider(a_menu, a_module, label) :
                                   filteredBaseLight ?
                                       DrawFilteredBaseLightSlider(a_menu, a_module, label) :
                                   a_module.settings.size() > 1 || !a_module.link.empty() ||
                                           std::ranges::any_of(a_module.settings, SliderTargetIgnoresLink) ?
                                        DrawGroupedSlider(a_menu, a_module, label) :
                                       DrawSlider(a_menu, a_module, label);
                if (!drawn)
                {
                    DrawDisplayText(
                        "unsupportedSliderSetting",
                        false,
                        { { "setting", filteredWeather ?
                                           "filteredWeatherAdjustments." + a_module.id :
                                       filteredLightingTemplate ?
                                           "filteredLightingTemplateAdjustments." + a_module.id :
                                       filteredBaseLight ?
                                           "filteredBaseLightAdjustments." + a_module.id :
                                           fallbackLabel } });
                }
                return;
            }
            if (a_module.type == "ambientWithinGauge" ||
                a_module.type == "ambientBetweenGauge" ||
                a_module.type == "sunlightWithinGauge" ||
                a_module.type == "sunlightBetweenGauge")
            {
                const bool sunlight = a_module.type == "sunlightWithinGauge" ||
                                      a_module.type == "sunlightBetweenGauge";
                DrawDynamicBrightnessModule(
                    a_menu,
                    a_module,
                    a_module.type == "ambientWithinGauge" ||
                            a_module.type == "sunlightWithinGauge" ?
                        WeatherPatcher::DynamicAmbientMode::within :
                        WeatherPatcher::DynamicAmbientMode::between,
                    sunlight ?
                        WeatherPatcher::DynamicBrightnessField::sunlight :
                        WeatherPatcher::DynamicBrightnessField::ambient);
                return;
            }
            if (a_module.type == "weatherSelector")
            {
                const auto label = (a_module.label.empty() ? "Weather" : a_module.label) +
                                   "##" + a_menu.profile + "MenuModule" + std::to_string(a_index);
                DrawWeatherSelector(a_menu, a_module, label);
                return;
            }
            if (a_module.type == "weatherControlCompact")
            {
                DrawWeatherControlCompact(
                    a_menu,
                    "##" + a_menu.profile + "WeatherControlCompact" + std::to_string(a_index));
                return;
            }
            if (a_module.type == "dynamicAmbientWeatherList")
            {
                DrawDynamicBrightnessWeatherList(
                    a_menu,
                    Config::IEquals(a_module.setting, "sunlight") ?
                        WeatherPatcher::DynamicBrightnessField::sunlight :
                        WeatherPatcher::DynamicBrightnessField::ambient);
                return;
            }
            if (a_module.type == "settings")
            {
                if (!DrawSettingsEditor(a_menu, a_module)) DrawUnsupportedSettingsEditor(a_module);
                return;
            }
            if (a_module.type == "links")
            {
                DrawLinksModule(a_menu, a_module);
                return;
            }
            if (a_module.type == "weatherSetup" || a_module.type == "interiorSetup")
            {
                DrawSetupModule(a_menu, a_module.type == "weatherSetup", a_index);
                return;
            }
            if (a_module.type == "weatherSliderCreator" || a_module.type == "interiorSliderCreator")
            {
                DrawSliderCreator(
                    a_menu,
                    a_module,
                    a_module.type == "interiorSliderCreator" ?
                        SliderCreatorDomain::interior :
                        SliderCreatorDomain::weather);
                return;
            }

            DrawDisplayText(
                "unsupportedModuleKind",
                false,
                { { "kind", a_module.type } });
        }
        void DrawItemTooltip(const std::string& a_tooltip)
        {
            ImGuiMCP::ImGuiHoveredFlags hoverFlags = ImGuiMCP::ImGuiHoveredFlags_ForTooltip;
            switch (SKSEMenuSettings::GetTooltipDelay())
            {
            case SKSEMenuSettings::TooltipDelay::none:
                hoverFlags |= ImGuiMCP::ImGuiHoveredFlags_DelayNone;
                break;
            case SKSEMenuSettings::TooltipDelay::shortDelay:
                hoverFlags |= ImGuiMCP::ImGuiHoveredFlags_DelayShort;
                break;
            default:
                hoverFlags |= ImGuiMCP::ImGuiHoveredFlags_DelayNormal;
                break;
            }
            if (!a_tooltip.empty() && ImGuiMCP::IsItemHovered(hoverFlags))
            {
                auto colorCount = 0;
                if (const auto color = SKSEMenuSettings::GetTooltipTextColor())
                {
                    ImGuiMCP::PushStyleColor(
                        ImGuiMCP::ImGuiCol_Text,
                        ImGuiMCP::ImVec4((*color)[0], (*color)[1], (*color)[2], (*color)[3]));
                    ++colorCount;
                }
                if (const auto color = SKSEMenuSettings::GetTooltipBackgroundColor())
                {
                    ImGuiMCP::PushStyleColor(
                        ImGuiMCP::ImGuiCol_PopupBg,
                        ImGuiMCP::ImVec4((*color)[0], (*color)[1], (*color)[2], (*color)[3]));
                    ++colorCount;
                }
                const auto visible = ImGuiMCP::BeginTooltip();
                if (visible)
                {
                    const auto scale = SKSEMenuSettings::GetTooltipFontScale();
                    if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(scale);
                    constexpr float tooltipWrapWidthInCharacters = 35.0f;
                    ImGuiMCP::PushTextWrapPos(
                        ImGuiMCP::GetCursorPosX() +
                        ImGuiMCP::GetFontSize() * tooltipWrapWidthInCharacters);
                    ImGuiMCP::TextUnformatted(a_tooltip.c_str());
                    ImGuiMCP::PopTextWrapPos();
                    if (scale != 1.0f) ImGuiMCP::SetWindowFontScale(1.0f);
                }
                ImGuiMCP::EndTooltip();
                if (colorCount > 0) ImGuiMCP::PopStyleColor(colorCount);
            }
        }

        float OverrideWarningSize()
        {
            return std::min(ImGuiMCP::GetFrameHeight(), 18.0f);
        }

        void DrawOverrideWarning(const std::string& a_profile, const std::string& a_id)
        {
            const auto size = OverrideWarningSize();
            ImGuiMCP::InvisibleButton(a_id.c_str(), ImGuiMCP::ImVec2(size, size));
            const auto minimum = ImGuiMCP::GetItemRectMin();
            const auto maximum = ImGuiMCP::GetItemRectMax();
            const auto centerX = (minimum.x + maximum.x) * 0.5f;
            auto* drawList = ImGuiMCP::GetWindowDrawList();
            const auto warningColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4(1.0f, 0.72f, 0.18f, 1.0f));
            const auto symbolColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4(0.12f, 0.10f, 0.05f, 1.0f));
            ImGuiMCP::ImDrawListManager::AddTriangleFilled(
                drawList,
                ImGuiMCP::ImVec2(centerX, minimum.y),
                ImGuiMCP::ImVec2(minimum.x, maximum.y),
                ImGuiMCP::ImVec2(maximum.x, maximum.y),
                warningColor);
            const auto symbolSize = ImGuiMCP::CalcTextSize("!");
            ImGuiMCP::ImDrawListManager::AddText(
                drawList,
                ImGuiMCP::ImVec2(
                    centerX - symbolSize.x * 0.5f,
                    minimum.y + (size - symbolSize.y) * 0.65f),
                symbolColor,
                "!");
            DrawItemTooltip(DisplayText("settingOverrideTooltip", { { "profile", a_profile } }));
        }

        void DrawModuleWithTooltip(
            const MenuDefinition& a_menu,
            const MenuControl& a_module,
            const std::span<const MenuControl> a_pageModules,
            const std::size_t a_index)
        {
            const std::span<const MenuControl> module(&a_module, 1);
            const auto settingPaths = PageResetScopes(a_menu, module);
            const auto revisionBefore = TuningUtil::GetSettingsRevision();
            const auto overridingProfile = TuningUtil::GetOverridingProfile(a_menu.profile, settingPaths);

            ImGuiMCP::BeginGroup();
            if (!settingPaths.empty())
            {
                if (overridingProfile)
                {
                    DrawOverrideWarning(
                        *overridingProfile,
                        "##SettingOverride" + a_menu.profile + std::to_string(a_index));
                }
                else
                {
                    const auto size = OverrideWarningSize();
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(size, size));
                }
                ImGuiMCP::SameLine();
            }
            ImGuiMCP::BeginGroup();
            DrawModule(a_menu, a_module, a_pageModules, a_index);
            ImGuiMCP::EndGroup();
            DrawItemTooltip(a_module.tooltip);
            ImGuiMCP::EndGroup();

            if (TuningUtil::GetSettingsRevision() != revisionBefore)
            {
                if (const auto currentOverride = TuningUtil::GetOverridingProfile(a_menu.profile, settingPaths))
                {
                    statusMessage = StatusText(
                        "settingOverrideChanged",
                        { { "profile", *currentOverride } });
                }
            }
        }

        void DrawPageModules(
            const MenuDefinition& a_menu,
            const std::span<const MenuControl> a_modules,
            const std::string_view a_pageID)
        {
            struct OpenBox
            {
                bool began = false;
                bool visible = false;
                bool customPadding = false;
            };
            std::vector<OpenBox> openBoxes;
            auto hasVisibleContent = false;
            auto profile = a_menu.profile;
            const auto advancedVisible = TuningUtil::GetSettings(profile).ShowAdvanced;
            const auto contentVisible = [&]()
            { return openBoxes.empty() || openBoxes.back().visible; };
            const auto addSpacing = [&]()
            {
                const auto spacing = SKSEMenuSettings::GetSectionSpacing();
                if (spacing > 0.0f) ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0.0f, spacing));
            };
            const auto closeBox = [&]()
            {
                const auto box = openBoxes.back();
                openBoxes.pop_back();
                if (!box.began) return;
                ImGuiMCP::EndChild();
                if (box.customPadding) ImGuiMCP::PopStyleVar();
            };

            for (std::size_t index = 0; index < a_modules.size(); ++index)
            {
                const auto& module = a_modules[index];
                if (module.advanced && !advancedVisible) continue;
                if (module.type == "boxStart")
                {
                    if (!contentVisible())
                    {
                        openBoxes.push_back({});
                        continue;
                    }
                    if (hasVisibleContent) addSpacing();
                    constexpr auto boxFlags = ImGuiMCP::ImGuiChildFlags_Border |
                                              ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                                              ImGuiMCP::ImGuiChildFlags_AutoResizeY;
                    const auto padding = SKSEMenuSettings::GetBoxPadding();
                    const auto customPadding = padding[0] > 0.0f || padding[1] > 0.0f;
                    if (customPadding)
                    {
                        ImGuiMCP::PushStyleVar(
                            ImGuiMCP::ImGuiStyleVar_WindowPadding,
                            ImGuiMCP::ImVec2(padding[0], padding[1]));
                    }
                    const auto boxID = "LayoutBox##" + a_menu.profile + std::string(a_pageID) + std::to_string(index);
                    const auto visible = ImGuiMCP::BeginChild(
                        boxID.c_str(),
                        ImGuiMCP::ImVec2(0.0f, 0.0f),
                        boxFlags);
                    openBoxes.push_back({ true, visible, customPadding });
                    const auto boxTitle = ControlDisplayName(module, module.label);
                    if (visible && !boxTitle.empty()) DrawHeader(boxTitle);
                    hasVisibleContent = true;
                    continue;
                }
                if (module.type == "boxEnd")
                {
                    if (!openBoxes.empty()) closeBox();
                    continue;
                }
                if (!contentVisible()) continue;
                if (hasVisibleContent) addSpacing();
                DrawModuleWithTooltip(a_menu, module, a_modules, index);
                hasVisibleContent = true;
            }
            while (!openBoxes.empty()) closeBox();
        }
        void BeginProfilePage(const std::string& a_profile, const std::string& a_page)
        {
            auto [active, inserted] = activeProfilePages.try_emplace(a_profile, a_page);
            if (!inserted && active->second != a_page)
            {
                active->second = a_page;
                if (SKSEMenuSettings::ClearStatusOnPageChange()) statusMessage.clear();
            }
            const auto spacing = SKSEMenuSettings::GetPageTopSpacing();
            if (spacing > 0.0f) ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0.0f, spacing));
        }

        struct LayoutModuleChoice
        {
            std::string_view name;
            std::string_view type;
            std::string_view setting;
            std::string_view defaultLabel;
        };

        constexpr std::array kLayoutModuleChoices{
            LayoutModuleChoice{ "Page Actions", "pageActions", "", "" },
            LayoutModuleChoice{ "Preset Save", "presetSave", "", "" },
            LayoutModuleChoice{ "Presets", "presets", "", "" },
            LayoutModuleChoice{ "Create Preset", "presetCreator", "", "" },
            LayoutModuleChoice{ "Dynamic Ambient Weather List", "dynamicAmbientWeatherList", "ambient", "" },
            LayoutModuleChoice{ "Dynamic Sunlight Weather List", "dynamicAmbientWeatherList", "sunlight", "" },
            LayoutModuleChoice{ "Weather Control Compact", "weatherControlCompact", "", "" },
            LayoutModuleChoice{ "Weather Selector", "weatherSelector", "", "" },
            LayoutModuleChoice{ "Weather Brightness", "settings", "brightness", "" },
            LayoutModuleChoice{ "Weather Saturation", "settings", "saturation", "" },
            LayoutModuleChoice{ "Weather Saturation Scales", "settings", "hueScales", "" },
            LayoutModuleChoice{ "Weather Hue Ranges", "settings", "hueRanges", "" },
            LayoutModuleChoice{ "Weather Hue Shift", "settings", "hueShift", "" },
            LayoutModuleChoice{ "Weather Compression Between", "settings", "betweenCompression", "" },
            LayoutModuleChoice{ "Weather Compression Within", "settings", "withinCompression", "" },
            LayoutModuleChoice{ "Ambient Brightness Between Weather Gauge", "ambientBetweenGauge", "", "" },
            LayoutModuleChoice{ "Ambient Brightness Within Weather Gauge", "ambientWithinGauge", "", "" },
            LayoutModuleChoice{ "Sunlight Brightness Between Weather Gauge", "sunlightBetweenGauge", "", "" },
            LayoutModuleChoice{ "Sunlight Brightness Within Weather Gauge", "sunlightWithinGauge", "", "" },
            LayoutModuleChoice{ "Weather Image Space", "settings", "exteriorImageSpace", "" },
            LayoutModuleChoice{ "Lighting Effects", "settings", "fxEffectLighting", "" },
            LayoutModuleChoice{ "Lighting Bulbs", "settings", "pointLights", "" },
            LayoutModuleChoice{ "Interior Brightness", "settings", "intBrightness", "" },
            LayoutModuleChoice{ "Interior Saturation Scales", "settings", "intHueScales", "" },
            LayoutModuleChoice{ "Interior Hue Shift", "settings", "intHueShift", "" },
            LayoutModuleChoice{ "Interior Image Space", "settings", "intImageSpace", "" },
            LayoutModuleChoice{ "Weather Links", "links", "weather", "" },
            LayoutModuleChoice{ "Interior Links", "links", "interior", "" },
            LayoutModuleChoice{ "Weather Setup", "weatherSetup", "", "" },
            LayoutModuleChoice{ "Lighting Setup", "interiorSetup", "", "" },
            LayoutModuleChoice{ "Weather Slider Creator", "weatherSliderCreator", "", "" },
            LayoutModuleChoice{ "Interior Slider Creator", "interiorSliderCreator", "", "" },
        };

        constexpr std::array kLayoutElementChoices{
            LayoutModuleChoice{ "Text", "text", "", "Text" },
            LayoutModuleChoice{ "Header", "separatorText", "", "Section" },
            LayoutModuleChoice{ "Separator", "separator", "", "" },
            LayoutModuleChoice{ "Space", "spacing", "", "" },
            LayoutModuleChoice{ "Box Start", "boxStart", "", "" },
            LayoutModuleChoice{ "Box End", "boxEnd", "", "" },
        };

        template <std::size_t Size>
        std::array<std::size_t, Size> AlphabeticalChoiceOrder(
            const std::array<LayoutModuleChoice, Size>& a_choices)
        {
            std::array<std::size_t, Size> order{};
            for (std::size_t index = 0; index < Size; ++index) order[index] = index;
            std::ranges::sort(order, [&](const auto a_left, const auto a_right)
                {
                const auto left = Lowercase(std::string(a_choices[a_left].name));
                const auto right = Lowercase(std::string(a_choices[a_right].name));
                return left != right ? left < right : a_choices[a_left].name < a_choices[a_right].name; });
            return order;
        }

        std::string LayoutModuleDisplayName(const MenuControl& a_module)
        {
            static constexpr std::array profileModules{
                std::pair{ std::string_view("profileActions"), std::string_view("Save / Restore / Reset") },
                std::pair{ std::string_view("enableProfile"), std::string_view("Enable Profile") },
                std::pair{ std::string_view("profilePriority"), std::string_view("Profile Priority") },
                std::pair{ std::string_view("advancedToggle"), std::string_view("Advanced Toggle") },
            };
            const auto typeName = a_module.type == "separatorText" ? "Header" : a_module.type;
            if (a_module.displayName && !a_module.displayName->empty())
            {
                return *a_module.displayName + " (" + typeName + ")";
            }
            if (!a_module.header.empty())
            {
                return a_module.header + " (" + typeName + ")";
            }
            if (!a_module.label.empty())
            {
                return a_module.label + " (" + typeName + ")";
            }
            if (const auto module = std::ranges::find_if(profileModules, [&](const auto& a_choice)
                    { return Config::IEquals(a_module.type, a_choice.first); });
                module != profileModules.end())
            {
                return std::string(module->second);
            }
            if (const auto choice = std::ranges::find_if(kLayoutModuleChoices, [&](const LayoutModuleChoice& a_choice)
                    { return a_module.type == a_choice.type && a_module.setting == a_choice.setting; });
                choice != kLayoutModuleChoices.end())
            {
                return std::string(choice->name);
            }
            if (const auto choice = std::ranges::find_if(kLayoutElementChoices, [&](const LayoutModuleChoice& a_choice)
                    { return a_module.type == a_choice.type && a_module.setting == a_choice.setting; });
                choice != kLayoutElementChoices.end())
            {
                return std::string(choice->name);
            }
            if (!a_module.setting.empty())
            {
                return a_module.setting + " (" + a_module.type + ")";
            }
            if (!a_module.id.empty())
            {
                return a_module.id + " (" + a_module.type + ")";
            }
            return a_module.type;
        }

        bool LayoutModuleHasDisplayName(const MenuControl& a_module)
        {
            static constexpr std::array types{
                std::string_view{ "slider" },
                std::string_view{ "text" },
                std::string_view{ "separatorText" },
                std::string_view{ "boxStart" },
                std::string_view{ "ambientWithinGauge" },
                std::string_view{ "ambientBetweenGauge" },
                std::string_view{ "sunlightWithinGauge" },
                std::string_view{ "sunlightBetweenGauge" },
                std::string_view{ "links" },
                std::string_view{ "presetCreator" },
            };
            return std::ranges::contains(types, std::string_view(a_module.type));
        }

        std::optional<std::string> LayoutModuleEditableName(const MenuControl& a_module)
        {
            if (!LayoutModuleHasDisplayName(a_module)) return std::nullopt;
            if (a_module.displayName) return *a_module.displayName;

            if (a_module.type == "slider")
            {
                if (!a_module.label.empty()) return a_module.label;
                if (!a_module.settings.empty()) return std::string(SliderTargetPath(a_module.settings.front()));
                if (!a_module.setting.empty()) return a_module.setting;
                return a_module.id;
            }
            if (a_module.type == "text" || a_module.type == "separatorText" || a_module.type == "boxStart")
            {
                return a_module.label;
            }
            if (a_module.type == "links")
            {
                return Config::IEquals(a_module.setting, "interior") ? "Interior Links" : "Weather Links";
            }
            if (a_module.type == "presetCreator")
            {
                return a_module.header.empty() ?
                           SKSEMenuSettings::Label("createPresetHeader", "Create Preset") :
                           a_module.header;
            }

            if (const auto choice = std::ranges::find_if(kLayoutModuleChoices, [&](const LayoutModuleChoice& a_choice)
                    { return a_module.type == a_choice.type && a_module.setting == a_choice.setting; });
                choice != kLayoutModuleChoices.end())
            {
                if (a_module.type == "ambientWithinGauge" ||
                    a_module.type == "ambientBetweenGauge" ||
                    a_module.type == "sunlightWithinGauge" ||
                    a_module.type == "sunlightBetweenGauge")
                {
                    return SKSEMenuSettings::Label(a_module.type, choice->name);
                }
                return std::string(choice->name);
            }
            return std::nullopt;
        }

        void QueueLayoutReload(std::string a_message)
        {
            pendingMenuReloadStatus = std::move(a_message);
            loadedDefinitionFiles.clear();
            nextDefinitionCheck = {};
        }

        void DrawLayoutModuleRenameControls(
            const MenuDefinition& a_menu,
            LayoutEditSession& a_session,
            LayoutEditorState& a_state,
            const MenuControl& a_module,
            const std::optional<std::size_t> a_pageIndex,
            const std::size_t a_moduleIndex)
        {
            const auto currentName = LayoutModuleEditableName(a_module);
            if (!currentName) return;

            const auto stateKey = std::format(
                "{}:{}:{}:{}:{}",
                a_menu.profile,
                a_pageIndex ? "page" : "profile",
                a_pageIndex.value_or(0),
                a_moduleIndex,
                a_module.type);
            auto& input = a_state.moduleNames[stateKey];
            if (input.source != *currentName)
            {
                SetInputText(input.value, *currentName);
                input.source = *currentName;
            }

            ImGuiMCP::TableSetColumnIndex(4);
            const auto renameLabel = SKSEMenuSettings::Label("renameModule", "Rename") +
                                     "##" + stateKey;
            if (ImGuiMCP::SmallButton(renameLabel.c_str()))
            {
                std::string error;
                const auto renamed = a_pageIndex ?
                                         SliderCreator::RenameModule(
                                             a_session.workingPath,
                                             *a_pageIndex,
                                             a_moduleIndex,
                                             InputText(input.value),
                                             error) :
                                         SliderCreator::RenameProfileModule(
                                             a_session.workingPath,
                                             a_moduleIndex,
                                             InputText(input.value),
                                             error);
                if (renamed)
                {
                    input.source.clear();
                    QueueLayoutEditReload(a_session, StatusText("layoutModuleRenamed"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }

            ImGuiMCP::TableSetColumnIndex(5);
            ImGuiMCP::SetNextItemWidth(260.0f);
            const auto inputLabel = "##ModuleName" + stateKey;
            ImGuiMCP::InputText(inputLabel.c_str(), input.value.data(), input.value.size());
        }

        void QueueLayoutEditReload(LayoutEditSession& a_session, std::string a_message)
        {
            a_session.dirty = true;
            QueueLayoutReload(std::move(a_message));
        }

        void DrawProfileLayoutEditActions(const LoadedMenu& a_menu, LayoutEditSession& a_session)
        {
            ImGuiMCP::BeginDisabled(!a_session.dirty);
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
                const auto label = SKSEMenuSettings::Label("saveProfileEdits", "Save Profile Edits") +
                                   "##" + a_menu.definition.profile;
                if (ImGuiMCP::Button(label.c_str()))
                {
                    std::string error;
                    if (SaveLayoutEditSession(a_session, error))
                        QueueLayoutReload(StatusText("layoutProfileSaved"));
                    else
                    {
                        logger::warn("[Tuning Menu] Edit Mode save failed | profile={} | {}", a_menu.definition.profile, error);
                        statusMessage = StatusText("layoutProfileSaveFailure");
                    }
                }
            }
            SameActionLine();
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::restore));
                const auto label = SKSEMenuSettings::Label("restoreProfileEdits", "Restore Profile Edits") +
                                   "##" + a_menu.definition.profile;
                if (ImGuiMCP::Button(label.c_str()))
                {
                    std::string error;
                    if (RestoreLayoutEditSession(a_session, error))
                        QueueLayoutReload(StatusText("layoutProfileRestored"));
                    else
                    {
                        logger::warn("[Tuning Menu] Edit Mode restore failed | profile={} | {}", a_menu.definition.profile, error);
                        statusMessage = StatusText("layoutProfileRestoreFailure");
                    }
                }
            }
            ImGuiMCP::EndDisabled();
        }

        void DrawPageLayoutEditActions(
            const LoadedMenu& a_menu,
            LayoutEditSession& a_session,
            const std::size_t a_pageIndex)
        {
            const auto validPage = a_pageIndex < a_session.pageOrigins.size();
            ImGuiMCP::BeginDisabled(!a_session.dirty || !validPage);
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
                const auto label = SKSEMenuSettings::Label("savePageEdits", "Save Page Edits") +
                                   "##" + a_menu.definition.profile + std::to_string(a_pageIndex);
                if (ImGuiMCP::Button(label.c_str()))
                {
                    std::string error;
                    auto savedPageIndex = std::size_t{};
                    if (SliderCreator::SavePageEdits(
                            a_session.workingPath,
                            a_session.sourcePath,
                            a_pageIndex,
                            a_session.pageOrigins[a_pageIndex],
                            savedPageIndex,
                            error))
                    {
                        a_session.pageOrigins[a_pageIndex] = savedPageIndex;
                        RefreshLayoutDirtyState(a_session);
                        QueueLayoutReload(StatusText("layoutPageSaved"));
                    }
                    else
                    {
                        logger::warn(
                            "[Tuning Menu] Edit Mode page save failed | page={} | profile={} | {}",
                            a_pageIndex,
                            a_menu.definition.profile,
                            error);
                        statusMessage = StatusText("layoutPageSaveFailure");
                    }
                }
            }
            SameActionLine();
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::restore));
                const auto label = SKSEMenuSettings::Label("restorePageEdits", "Restore Page Edits") +
                                   "##" + a_menu.definition.profile + std::to_string(a_pageIndex);
                if (ImGuiMCP::Button(label.c_str()))
                {
                    std::string error;
                    const auto hadSavedPage = a_session.pageOrigins[a_pageIndex].has_value();
                    if (SliderCreator::RestorePageEdits(
                            a_session.sourcePath,
                            a_session.workingPath,
                            a_pageIndex,
                            a_session.pageOrigins[a_pageIndex],
                            error))
                    {
                        if (!hadSavedPage)
                        {
                            a_session.pageOrigins.erase(a_session.pageOrigins.begin() + a_pageIndex);
                            if (a_session.pageOrigins.empty())
                                requestedAutomaticProfilePages.insert(a_menu.definition.profile);
                            else
                                requestedProfilePageIndices[a_menu.definition.profile] =
                                    std::min(a_pageIndex, a_session.pageOrigins.size() - 1);
                        }
                        RefreshLayoutDirtyState(a_session);
                        QueueLayoutReload(StatusText("layoutPageRestored"));
                    }
                    else
                    {
                        logger::warn(
                            "[Tuning Menu] Edit Mode page restore failed | page={} | profile={} | {}",
                            a_pageIndex,
                            a_menu.definition.profile,
                            error);
                        statusMessage = StatusText("layoutPageRestoreFailure");
                    }
                }
            }
            ImGuiMCP::EndDisabled();
        }

        void DrawAllLayoutEditActions()
        {
            const auto hasChanges = std::ranges::any_of(
                layoutEditSessions,
                [](const auto& a_entry) { return a_entry.second.dirty; });
            ImGuiMCP::BeginDisabled(!hasChanges);
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
                const auto label = SKSEMenuSettings::Label("saveAllEdits", "Save All Edits") + "##TuningSettings";
                if (ImGuiMCP::Button(label.c_str()))
                {
                    std::string error;
                    auto success = true;
                    for (auto& [profile, session] : layoutEditSessions)
                    {
                        if (!session.dirty || SaveLayoutEditSession(session, error)) continue;
                        logger::warn("[Tuning Menu] Edit Mode save failed | profile={} | {}", profile, error);
                        success = false;
                    }
                    if (success)
                    {
                        loadedDefinitionFiles.clear();
                        nextDefinitionCheck = {};
                        settingsStatusMessage = StatusText("layoutAllSaved");
                    }
                    else
                        settingsStatusMessage = StatusText("layoutAllSaveFailure");
                }
            }
            SameActionLine();
            {
                const ButtonColorStyle color(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::restore));
                const auto label = SKSEMenuSettings::Label("restoreAllEdits", "Restore All Edits") + "##TuningSettings";
                if (ImGuiMCP::Button(label.c_str()))
                {
                    std::string error;
                    auto success = true;
                    for (auto& [profile, session] : layoutEditSessions)
                    {
                        if (!session.dirty || RestoreLayoutEditSession(session, error)) continue;
                        logger::warn("[Tuning Menu] Edit Mode restore failed | profile={} | {}", profile, error);
                        success = false;
                    }
                    if (success)
                    {
                        loadedDefinitionFiles.clear();
                        nextDefinitionCheck = {};
                        settingsStatusMessage = StatusText("layoutAllRestored");
                    }
                    else
                        settingsStatusMessage = StatusText("layoutAllRestoreFailure");
                }
            }
            ImGuiMCP::EndDisabled();
        }

        class EditModePanel
        {
        public:
            explicit EditModePanel(const std::string& a_id)
            {
                ImGuiMCP::Spacing();
                constexpr auto flags = ImGuiMCP::ImGuiChildFlags_Border |
                                       ImGuiMCP::ImGuiChildFlags_AlwaysUseWindowPadding |
                                       ImGuiMCP::ImGuiChildFlags_AutoResizeY;
                const auto padding = SKSEMenuSettings::GetBoxPadding();
                customPadding = padding[0] > 0.0f || padding[1] > 0.0f;
                if (customPadding)
                {
                    ImGuiMCP::PushStyleVar(
                        ImGuiMCP::ImGuiStyleVar_WindowPadding,
                        ImGuiMCP::ImVec2(padding[0], padding[1]));
                }
                visible = ImGuiMCP::BeginChild(a_id.c_str(), ImGuiMCP::ImVec2(0.0f, 0.0f), flags);
            }

            ~EditModePanel()
            {
                ImGuiMCP::EndChild();
                if (customPadding) ImGuiMCP::PopStyleVar();
            }

            explicit operator bool() const { return visible; }

            EditModePanel(const EditModePanel&) = delete;
            EditModePanel& operator=(const EditModePanel&) = delete;

        private:
            bool visible = false;
            bool customPadding = false;
        };

        void DrawPromoteUserSettingsAction(const LoadedMenu& a_menu)
        {
            auto profile = a_menu.definition.profile;
            const auto popupTitle =
                SKSEMenuSettings::Label(
                    "makeUserSettingsPermanentTitle",
                    "Make User Settings Permanent?") +
                "##" + profile;
            {
                const ButtonColorStyle color(
                    SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                const auto label =
                    SKSEMenuSettings::Label(
                        "makeUserSettingsPermanent",
                        "Make User Settings Permanent") +
                    "##" + profile;
                if (ImGuiMCP::Button(label.c_str())) ImGuiMCP::OpenPopup(popupTitle.c_str());
            }

            ImGuiMCP::SetNextWindowSize(
                ImGuiMCP::ImVec2(480.0f, 0.0f),
                ImGuiMCP::ImGuiCond_Appearing);
            if (!ImGuiMCP::BeginPopupModal(
                    popupTitle.c_str(),
                    nullptr,
                    ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize))
                return;

            ImGuiMCP::PushTextWrapPos(440.0f);
            ImGuiMCP::TextWrapped("%s", DisplayText("promoteUserSettingsConfirmation").c_str());
            ImGuiMCP::PopTextWrapPos();
            {
                const ButtonColorStyle color(
                    SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                const auto confirmLabel =
                    SKSEMenuSettings::Label(
                        "confirmMakeUserSettingsPermanent",
                        "Make Permanent") +
                    "##" + profile;
                if (ImGuiMCP::Button(confirmLabel.c_str()))
                {
                    std::string error;
                    const auto promoted = TuningUtil::PromoteUserSettingsToProfile(profile, error);
                    if (promoted)
                    {
                        profilePriorityInputs[profile] = TuningUtil::GetSettings(profile).profilePriority;
                        RefreshProfileMenuState(a_menu.definition);
                    }
                    statusMessage = promoted ?
                                        StatusText("userSettingsPromoted", { { "profile", a_menu.definition.title } }) :
                                        StatusText("userSettingsPromoteFailure", { { "reason", error } });
                    ImGuiMCP::CloseCurrentPopup();
                }
            }
            SameActionLine();
            const auto cancelLabel =
                SKSEMenuSettings::Label(
                    "cancelMakeUserSettingsPermanent",
                    "Cancel") +
                "##" + profile;
            if (ImGuiMCP::Button(cancelLabel.c_str())) ImGuiMCP::CloseCurrentPopup();
            ImGuiMCP::EndPopup();
        }

        void DrawProfilePageEditor(const LoadedMenu& a_menu)
        {
            std::string error;
            auto* editSession = EnsureLayoutEditSession(a_menu.definition.profile, a_menu.path, error);
            if (!editSession)
            {
                logger::warn("[Tuning Menu] Profile Edit Mode open failed | profile={} | {}", a_menu.definition.profile, error);
                statusMessage = StatusText("layoutEditSessionFailure");
                return;
            }

            const auto& profilePage = a_menu.definition.profilePage;
            auto& state = layoutEditorStates[a_menu.definition.profile];
            const EditModePanel panel("ProfilePageEditMode##" + a_menu.definition.profile);
            if (!panel) return;
            DrawHeader(SKSEMenuSettings::Label("editPage", "Edit Page"));
            DrawProfileLayoutEditActions(a_menu, *editSession);
            ImGuiMCP::Separator();
            DrawPromoteUserSettingsAction(a_menu);

            ImGuiMCP::TextUnformatted(SKSEMenuSettings::Label("pageOrder", "Page Order").c_str());
            ImGuiMCP::BeginDisabled(profilePage.order == 0);
            const auto movePageUpLabel = SKSEMenuSettings::Label("movePageUp", "Move Page Up") +
                                         "##" + a_menu.definition.profile + "ProfilePage";
            if (ImGuiMCP::Button(movePageUpLabel.c_str()))
            {
                std::string editError;
                if (SliderCreator::MoveProfilePage(editSession->workingPath, -1, editError))
                {
                    requestedAutomaticProfilePages.insert(a_menu.definition.profile);
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageMoved"));
                }
                else statusMessage = SliderCreatorErrorText(editError);
            }
            ImGuiMCP::EndDisabled();
            ImGuiMCP::SameLine();
            ImGuiMCP::BeginDisabled(profilePage.order >= a_menu.definition.pages.size());
            const auto movePageDownLabel = SKSEMenuSettings::Label("movePageDown", "Move Page Down") +
                                           "##" + a_menu.definition.profile + "ProfilePage";
            if (ImGuiMCP::Button(movePageDownLabel.c_str()))
            {
                std::string editError;
                if (SliderCreator::MoveProfilePage(editSession->workingPath, 1, editError))
                {
                    requestedAutomaticProfilePages.insert(a_menu.definition.profile);
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageMoved"));
                }
                else statusMessage = SliderCreatorErrorText(editError);
            }
            ImGuiMCP::EndDisabled();

            DrawHeader(SKSEMenuSettings::Label("createPageSection", "Create Page"));
            ImGuiMCP::SetNextItemWidth(260.0f);
            const auto newPageLabel = SKSEMenuSettings::Label("newPage", "New Page") +
                                      "##" + a_menu.definition.profile;
            ImGuiMCP::InputTextWithHint(
                newPageLabel.c_str(),
                "Page name",
                state.newPageName.data(),
                state.newPageName.size());
            const auto addPageLabel = SKSEMenuSettings::Label("addPage", "Add Page") +
                                      "##" + a_menu.definition.profile;
            if (ImGuiMCP::Button(addPageLabel.c_str()))
            {
                std::string editError;
                const auto title = InputText(state.newPageName);
                if (const auto newPage = SliderCreator::CreatePage(editSession->workingPath, title, editError))
                {
                    editSession->pageOrigins.insert(
                        editSession->pageOrigins.begin() +
                            std::min(*newPage, editSession->pageOrigins.size()),
                        std::nullopt);
                    state.newPageName.fill('\0');
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageAdded"));
                }
                else statusMessage = SliderCreatorErrorText(editError);
            }

            DrawHeader(SKSEMenuSettings::Label("addElement", "Add Element"));
            state.elementChoice = std::clamp(state.elementChoice, 0, static_cast<int>(kLayoutElementChoices.size() - 1));
            const auto& selectedElement = kLayoutElementChoices[static_cast<std::size_t>(state.elementChoice)];
            const auto elementLabel = SKSEMenuSettings::Label("element", "Element") +
                                      "##" + a_menu.definition.profile + "ProfilePage";
            if (ImGuiMCP::BeginCombo(
                    elementLabel.c_str(),
                    selectedElement.name.data(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                for (const auto index : AlphabeticalChoiceOrder(kLayoutElementChoices))
                {
                    const auto active = index == static_cast<std::size_t>(state.elementChoice);
                    if (ImGuiMCP::Selectable(kLayoutElementChoices[index].name.data(), active))
                    {
                        state.elementChoice = static_cast<int>(index);
                    }
                    if (active) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndCombo();
            }
            const auto elementUsesText = selectedElement.type == "text" ||
                                         selectedElement.type == "separatorText" ||
                                         selectedElement.type == "boxStart";
            if (elementUsesText)
            {
                ImGuiMCP::SetNextItemWidth(260.0f);
                const auto textLabel = SKSEMenuSettings::Label("elementText", "Text") +
                                       "##" + a_menu.definition.profile + "ProfilePage";
                ImGuiMCP::InputTextWithHint(
                    textLabel.c_str(),
                    selectedElement.defaultLabel.data(),
                    state.elementText.data(),
                    state.elementText.size());
            }
            const auto addSelectedElementLabel = SKSEMenuSettings::Label("addSelectedElement", "Add Selected Element") +
                                                 "##" + a_menu.definition.profile + "ProfilePage";
            if (ImGuiMCP::Button(addSelectedElementLabel.c_str()))
            {
                std::string editError;
                auto text = elementUsesText ? InputText(state.elementText) : std::string{};
                if (text.empty() && elementUsesText) text = std::string(selectedElement.defaultLabel);
                if (SliderCreator::AddProfileElement(
                        editSession->workingPath,
                        std::string(selectedElement.type),
                        text,
                        editError))
                {
                    state.elementText.fill('\0');
                    QueueLayoutEditReload(*editSession, StatusText("layoutElementAdded"));
                }
                else statusMessage = SliderCreatorErrorText(editError);
            }

            DrawHeader(SKSEMenuSettings::Label("pageContents", "Page Contents"));
            auto contentWidth = ImGuiMCP::CalcTextSize("Content").x;
            for (std::size_t index = 0; index < profilePage.modules.size(); ++index)
            {
                const auto display = std::format("{}. {}", index + 1, LayoutModuleDisplayName(profilePage.modules[index]));
                contentWidth = std::max(contentWidth, ImGuiMCP::CalcTextSize(display.c_str()).x);
            }
            constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_SizingFixedFit |
                                        ImGuiMCP::ImGuiTableFlags_NoSavedSettings |
                                        ImGuiMCP::ImGuiTableFlags_NoPadOuterX;
            const auto tableID = "ProfilePageContents##" + a_menu.definition.profile;
            if (ImGuiMCP::BeginTable(tableID.c_str(), 6, tableFlags))
            {
                ImGuiMCP::TableSetupColumn("Content", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, contentWidth);
                ImGuiMCP::TableSetupColumn("Up", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Down", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Remove", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Rename", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 260.0f);
                for (std::size_t index = 0; index < profilePage.modules.size(); ++index)
                {
                    const auto& module = profilePage.modules[index];
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    const auto display = std::format("{}. {}", index + 1, LayoutModuleDisplayName(module));
                    ImGuiMCP::TextUnformatted(display.c_str());

                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::BeginDisabled(index == 0);
                    const auto moveUpLabel = SKSEMenuSettings::Label("moveUp", "Up") +
                                             "##" + a_menu.definition.profile + "ProfilePage" + std::to_string(index);
                    if (ImGuiMCP::SmallButton(moveUpLabel.c_str()))
                    {
                        std::string editError;
                        if (SliderCreator::MoveProfileModule(editSession->workingPath, index, -1, editError))
                            QueueLayoutEditReload(*editSession, StatusText("layoutModuleMoved"));
                        else statusMessage = SliderCreatorErrorText(editError);
                    }
                    ImGuiMCP::EndDisabled();

                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::BeginDisabled(index + 1 >= profilePage.modules.size());
                    const auto moveDownLabel = SKSEMenuSettings::Label("moveDown", "Down") +
                                               "##" + a_menu.definition.profile + "ProfilePage" + std::to_string(index);
                    if (ImGuiMCP::SmallButton(moveDownLabel.c_str()))
                    {
                        std::string editError;
                        if (SliderCreator::MoveProfileModule(editSession->workingPath, index, 1, editError))
                            QueueLayoutEditReload(*editSession, StatusText("layoutModuleMoved"));
                        else statusMessage = SliderCreatorErrorText(editError);
                    }
                    ImGuiMCP::EndDisabled();

                    ImGuiMCP::TableSetColumnIndex(3);
                    const auto required = SliderCreator::IsRequiredProfileModuleKind(module.type);
                    ImGuiMCP::BeginDisabled(required);
                    const ButtonColorStyle destructiveColor(
                        SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                    const auto removeLabel = SKSEMenuSettings::Label("removeModule", "Remove") +
                                             "##" + a_menu.definition.profile + "ProfilePage" + std::to_string(index);
                    if (ImGuiMCP::SmallButton(removeLabel.c_str()))
                    {
                        std::string editError;
                        if (SliderCreator::RemoveProfileModule(editSession->workingPath, index, editError))
                            QueueLayoutEditReload(*editSession, StatusText("layoutModuleRemoved"));
                        else statusMessage = SliderCreatorErrorText(editError);
                    }
                    ImGuiMCP::EndDisabled();
                    DrawLayoutModuleRenameControls(
                        a_menu.definition,
                        *editSession,
                        state,
                        module,
                        std::nullopt,
                        index);
                }
                ImGuiMCP::EndTable();
            }
            ImGuiMCP::Separator();
        }

        void DrawLayoutEditor(
            const LoadedMenu& a_menu,
            const std::size_t a_pageIndex,
            const MenuPage& a_page)
        {
            std::string error;
            auto* editSession = EnsureLayoutEditSession(a_menu.definition.profile, a_menu.path, error);
            if (!editSession)
            {
                logger::warn("[Tuning Menu] Edit Mode open failed | profile={} | {}", a_menu.definition.profile, error);
                statusMessage = StatusText("layoutEditSessionFailure");
                return;
            }
            auto& state = layoutEditorStates[a_menu.definition.profile];
            const EditModePanel panel(
                "PageEditMode##" + a_menu.definition.profile + std::to_string(a_pageIndex));
            if (!panel) return;
            DrawHeader(SKSEMenuSettings::Label("editPage", "Edit Page"));
            DrawPageLayoutEditActions(a_menu, *editSession, a_pageIndex);

            ImGuiMCP::TextUnformatted(SKSEMenuSettings::Label("pageOrder", "Page Order").c_str());
            const auto profilePageOrder = a_menu.definition.profilePage.order;
            const auto pagePosition = a_pageIndex + (a_pageIndex >= profilePageOrder ? 1 : 0);
            ImGuiMCP::BeginDisabled(pagePosition == 0);
            const auto movePageUpLabel = SKSEMenuSettings::Label("movePageUp", "Move Page Up") +
                                         "##" + a_menu.definition.profile;
            if (ImGuiMCP::Button(movePageUpLabel.c_str()))
            {
                std::string error;
                const auto crossesProfilePage = pagePosition - 1 == profilePageOrder;
                const auto moved = crossesProfilePage ?
                                       SliderCreator::MoveProfilePage(editSession->workingPath, 1, error) :
                                       SliderCreator::MovePage(editSession->workingPath, a_pageIndex, -1, error);
                if (moved)
                {
                    if (!crossesProfilePage)
                    {
                        std::ranges::swap(
                            editSession->pageOrigins[a_pageIndex],
                            editSession->pageOrigins[a_pageIndex - 1]);
                    }
                    requestedProfilePageIndices[a_menu.definition.profile] =
                        crossesProfilePage ? a_pageIndex : a_pageIndex - 1;
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageMoved"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }
            ImGuiMCP::EndDisabled();
            ImGuiMCP::SameLine();
            ImGuiMCP::BeginDisabled(pagePosition >= a_menu.definition.pages.size());
            const auto movePageDownLabel = SKSEMenuSettings::Label("movePageDown", "Move Page Down") +
                                           "##" + a_menu.definition.profile;
            if (ImGuiMCP::Button(movePageDownLabel.c_str()))
            {
                std::string error;
                const auto crossesProfilePage = pagePosition + 1 == profilePageOrder;
                const auto moved = crossesProfilePage ?
                                       SliderCreator::MoveProfilePage(editSession->workingPath, -1, error) :
                                       SliderCreator::MovePage(editSession->workingPath, a_pageIndex, 1, error);
                if (moved)
                {
                    if (!crossesProfilePage)
                    {
                        std::ranges::swap(
                            editSession->pageOrigins[a_pageIndex],
                            editSession->pageOrigins[a_pageIndex + 1]);
                    }
                    requestedProfilePageIndices[a_menu.definition.profile] =
                        crossesProfilePage ? a_pageIndex : a_pageIndex + 1;
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageMoved"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }
            ImGuiMCP::EndDisabled();
            {
                ImGuiMCP::SameLine();
                const ButtonColorStyle destructiveColor(
                    SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                const auto deletePageLabel = SKSEMenuSettings::Label("deletePage", "Delete Page") +
                                             "##" + a_menu.definition.profile;
                if (ImGuiMCP::Button(deletePageLabel.c_str()))
                {
                    std::string error;
                    if (SliderCreator::RemovePage(editSession->workingPath, a_pageIndex, error))
                    {
                        editSession->pageOrigins.erase(editSession->pageOrigins.begin() + a_pageIndex);
                        QueueLayoutEditReload(*editSession, StatusText("layoutPageRemoved"));
                    }
                    else statusMessage = SliderCreatorErrorText(error);
                }
            }

            auto pageAdvanced = a_page.advanced;
            const auto pageAdvancedLabel = SKSEMenuSettings::Label(
                                               "advancedPage",
                                               "Page is Advanced") +
                                           "##" + a_menu.definition.profile +
                                           std::to_string(a_pageIndex);
            if (ImGuiMCP::Checkbox(pageAdvancedLabel.c_str(), &pageAdvanced))
            {
                std::string error;
                if (SliderCreator::SetPageAdvanced(
                        editSession->workingPath,
                        a_pageIndex,
                        pageAdvanced,
                        error))
                {
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageAdvancedChanged"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }

            const auto pageNameSource = std::format("{}:{}", a_pageIndex, a_page.title);
            if (state.pageNameSource != pageNameSource)
            {
                state.pageName.fill('\0');
                std::ranges::copy_n(
                    a_page.title.begin(),
                    std::min(a_page.title.size(), state.pageName.size() - 1),
                    state.pageName.begin());
                state.pageNameSource = pageNameSource;
            }
            ImGuiMCP::TextUnformatted(SKSEMenuSettings::Label("renamePageSection", "Rename Page").c_str());
            ImGuiMCP::SetNextItemWidth(260.0f);
            const auto pageNameLabel = SKSEMenuSettings::Label("pageName", "Page Name") +
                                       "##" + a_menu.definition.profile + std::to_string(a_pageIndex);
            ImGuiMCP::InputText(
                pageNameLabel.c_str(),
                state.pageName.data(),
                state.pageName.size());
            const auto renamePageLabel = SKSEMenuSettings::Label("renamePage", "Rename Page") +
                                         "##" + a_menu.definition.profile + std::to_string(a_pageIndex);
            if (ImGuiMCP::Button(renamePageLabel.c_str()))
            {
                std::string error;
                if (SliderCreator::RenamePage(
                        editSession->workingPath,
                        a_pageIndex,
                        InputText(state.pageName),
                        error))
                {
                    state.pageNameSource.clear();
                    QueueLayoutEditReload(*editSession, StatusText("layoutPageRenamed"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }

            DrawHeader(SKSEMenuSettings::Label("addElement", "Add Element"));
            state.elementChoice = std::clamp(state.elementChoice, 0, static_cast<int>(kLayoutElementChoices.size() - 1));
            const auto& selectedElement = kLayoutElementChoices[static_cast<std::size_t>(state.elementChoice)];
            const auto elementLabel = SKSEMenuSettings::Label("element", "Element") +
                                      "##" + a_menu.definition.profile;
            if (ImGuiMCP::BeginCombo(
                    elementLabel.c_str(),
                    selectedElement.name.data(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                for (const auto index : AlphabeticalChoiceOrder(kLayoutElementChoices))
                {
                    const auto active = index == static_cast<std::size_t>(state.elementChoice);
                    if (ImGuiMCP::Selectable(kLayoutElementChoices[index].name.data(), active))
                    {
                        state.elementChoice = static_cast<int>(index);
                    }
                    if (active) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndCombo();
            }
            const auto elementUsesText = selectedElement.type == "text" ||
                                         selectedElement.type == "separatorText" ||
                                         selectedElement.type == "boxStart";
            if (elementUsesText)
            {
                ImGuiMCP::SetNextItemWidth(260.0f);
                const auto textLabel = SKSEMenuSettings::Label("elementText", "Text") +
                                       "##" + a_menu.definition.profile;
                ImGuiMCP::InputTextWithHint(
                    textLabel.c_str(),
                    selectedElement.defaultLabel.data(),
                    state.elementText.data(),
                    state.elementText.size());
            }
            const auto addSelectedElementLabel = SKSEMenuSettings::Label("addSelectedElement", "Add Selected Element") +
                                                 "##" + a_menu.definition.profile;
            if (ImGuiMCP::Button(addSelectedElementLabel.c_str()))
            {
                std::string error;
                auto text = elementUsesText ? InputText(state.elementText) : std::string{};
                if (text.empty() && elementUsesText) text = std::string(selectedElement.defaultLabel);
                if (SliderCreator::AddModule(
                        editSession->workingPath,
                        a_pageIndex,
                        std::string(selectedElement.type),
                        text,
                        std::string(selectedElement.setting),
                        false,
                        error))
                {
                    state.elementText.fill('\0');
                    QueueLayoutEditReload(*editSession, StatusText("layoutElementAdded"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }

            DrawHeader(SKSEMenuSettings::Label("addModule", "Add Module"));
            state.moduleChoice = std::clamp(state.moduleChoice, 0, static_cast<int>(kLayoutModuleChoices.size() - 1));
            const auto& selectedModule = kLayoutModuleChoices[static_cast<std::size_t>(state.moduleChoice)];
            const auto moduleLabel = SKSEMenuSettings::Label("module", "Module") +
                                     "##" + a_menu.definition.profile;
            if (ImGuiMCP::BeginCombo(
                    moduleLabel.c_str(),
                    selectedModule.name.data(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                for (const auto index : AlphabeticalChoiceOrder(kLayoutModuleChoices))
                {
                    const auto active = index == static_cast<std::size_t>(state.moduleChoice);
                    if (ImGuiMCP::Selectable(kLayoutModuleChoices[index].name.data(), active))
                    {
                        state.moduleChoice = static_cast<int>(index);
                    }
                    if (active) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndCombo();
            }
            const auto moduleAdvancedLabel = SKSEMenuSettings::Label("advancedModule", "Module is Advanced") +
                                             "##" + a_menu.definition.profile;
            ImGuiMCP::Checkbox(moduleAdvancedLabel.c_str(), &state.moduleAdvanced);
            const auto addSelectedModuleLabel = SKSEMenuSettings::Label("addSelectedModule", "Add Selected Module") +
                                                "##" + a_menu.definition.profile;
            if (ImGuiMCP::Button(addSelectedModuleLabel.c_str()))
            {
                std::string error;
                if (SliderCreator::AddModule(
                        editSession->workingPath,
                        a_pageIndex,
                        std::string(selectedModule.type),
                        {},
                        std::string(selectedModule.setting),
                        state.moduleAdvanced,
                        error))
                {
                    QueueLayoutEditReload(*editSession, StatusText("layoutModuleAdded"));
                }
                else statusMessage = SliderCreatorErrorText(error);
            }

            DrawHeader(SKSEMenuSettings::Label("pageContents", "Page Contents"));
            auto contentWidth = ImGuiMCP::CalcTextSize("Content").x;
            for (std::size_t index = 0; index < a_page.modules.size(); ++index)
            {
                const auto display = std::format("{}. {}", index + 1, LayoutModuleDisplayName(a_page.modules[index]));
                contentWidth = std::max(contentWidth, ImGuiMCP::CalcTextSize(display.c_str()).x);
            }
            constexpr auto tableFlags = ImGuiMCP::ImGuiTableFlags_SizingFixedFit |
                                        ImGuiMCP::ImGuiTableFlags_NoSavedSettings |
                                        ImGuiMCP::ImGuiTableFlags_NoPadOuterX;
            const auto tableID = "PageContents##" + a_menu.definition.profile + std::to_string(a_pageIndex);
            if (ImGuiMCP::BeginTable(tableID.c_str(), 6, tableFlags))
            {
                ImGuiMCP::TableSetupColumn("Content", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, contentWidth);
                ImGuiMCP::TableSetupColumn("Up", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Down", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Remove", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Rename", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed);
                ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 260.0f);
                for (std::size_t index = 0; index < a_page.modules.size(); ++index)
                {
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    const auto display = std::format("{}. {}", index + 1, LayoutModuleDisplayName(a_page.modules[index]));
                    ImGuiMCP::TextUnformatted(display.c_str());

                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::BeginDisabled(index == 0);
                    const auto moveUpLabel = SKSEMenuSettings::Label("moveUp", "Up") +
                                             "##" + a_menu.definition.profile + std::to_string(index);
                    if (ImGuiMCP::SmallButton(moveUpLabel.c_str()))
                    {
                        std::string error;
                        if (SliderCreator::MoveModule(editSession->workingPath, a_pageIndex, index, -1, error))
                            QueueLayoutEditReload(*editSession, StatusText("layoutModuleMoved"));
                        else statusMessage = SliderCreatorErrorText(error);
                    }
                    ImGuiMCP::EndDisabled();

                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::BeginDisabled(index + 1 >= a_page.modules.size());
                    const auto moveDownLabel = SKSEMenuSettings::Label("moveDown", "Down") +
                                               "##" + a_menu.definition.profile + std::to_string(index);
                    if (ImGuiMCP::SmallButton(moveDownLabel.c_str()))
                    {
                        std::string error;
                        if (SliderCreator::MoveModule(editSession->workingPath, a_pageIndex, index, 1, error))
                            QueueLayoutEditReload(*editSession, StatusText("layoutModuleMoved"));
                        else statusMessage = SliderCreatorErrorText(error);
                    }
                    ImGuiMCP::EndDisabled();

                    ImGuiMCP::TableSetColumnIndex(3);
                    const ButtonColorStyle destructiveColor(
                        SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                    const auto removeLabel = SKSEMenuSettings::Label("removeModule", "Remove") +
                                             "##" + a_menu.definition.profile + std::to_string(index);
                    if (ImGuiMCP::SmallButton(removeLabel.c_str()))
                    {
                        std::string error;
                        if (SliderCreator::RemoveModule(editSession->workingPath, a_pageIndex, index, error))
                            QueueLayoutEditReload(*editSession, StatusText("layoutModuleRemoved"));
                        else statusMessage = SliderCreatorErrorText(error);
                    }
                    DrawLayoutModuleRenameControls(
                        a_menu.definition,
                        *editSession,
                        state,
                        a_page.modules[index],
                        a_pageIndex,
                        index);
                }
                ImGuiMCP::EndTable();
            }
            ImGuiMCP::Separator();
        }

        void DrawProfileMenu(const LoadedMenu& a_menu)
        {
            const auto& definition = a_menu.definition;
            auto profile = definition.profile;
            const auto advancedVisible = TuningUtil::GetSettings(profile).ShowAdvanced;
            const auto tabBarId = "LumaProfileSubpages##" + definition.profile +
                                  std::to_string(definition.profilePage.order);
            if (!ImGuiMCP::BeginTabBar(tabBarId.c_str())) return;

            const auto drawProfilePage = [&]
            {
                const auto profileTabLabel = "Profile##" + definition.profile + "ProfilePage";
                const auto requestedProfilePage = requestedAutomaticProfilePages.contains(definition.profile);
                const auto tabFlags = requestedProfilePage ?
                                          ImGuiMCP::ImGuiTabItemFlags_SetSelected :
                                          ImGuiMCP::ImGuiTabItemFlags_None;
                if (!ImGuiMCP::BeginTabItem(profileTabLabel.c_str(), nullptr, tabFlags)) return;
                if (requestedProfilePage) requestedAutomaticProfilePages.erase(definition.profile);
                BeginProfilePage(definition.profile, "Profile");
                if (SKSEMenuSettings::GetStatusLocation() == SKSEMenuSettings::StatusLocation::top)
                {
                    DrawStatusMessage(statusMessage, "LumaTopStatus##" + definition.profile + "Profile");
                }

                if (editModeEnabled && !definition.lockEditMode)
                {
                    DrawProfilePageEditor(a_menu);
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0.0f, ImGuiMCP::GetFrameHeight()));
                }
                DrawPageModules(definition, definition.profilePage.modules, "Profile");
                ImGuiMCP::EndTabItem();
            };

            const auto drawCustomPage = [&](const std::size_t a_pageIndex)
            {
                const auto& page = definition.pages[a_pageIndex];
                if (page.advanced && !advancedVisible)
                {
                    return;
                }
                const auto tabLabel = (page.title.empty() ? "Page" : page.title) + "##" +
                                      definition.profile + "Subpage" + std::to_string(a_pageIndex);
                const auto requestedPage =
                    requestedProfilePageIndices.find(definition.profile);
                const auto tabFlags =
                    requestedPage != requestedProfilePageIndices.end() &&
                            requestedPage->second == a_pageIndex ?
                        ImGuiMCP::ImGuiTabItemFlags_SetSelected :
                        ImGuiMCP::ImGuiTabItemFlags_None;
                if (!ImGuiMCP::BeginTabItem(
                        tabLabel.c_str(),
                        nullptr,
                        tabFlags))
                {
                    return;
                }
                if (tabFlags == ImGuiMCP::ImGuiTabItemFlags_SetSelected)
                {
                    requestedProfilePageIndices.erase(requestedPage);
                }

                BeginProfilePage(definition.profile, std::to_string(a_pageIndex));
                if (SKSEMenuSettings::GetStatusLocation() == SKSEMenuSettings::StatusLocation::top)
                {
                    DrawStatusMessage(
                        statusMessage,
                        "LumaTopStatus##" + definition.profile + std::to_string(a_pageIndex));
                }
                if (!page.description.empty())
                {
                    ImGuiMCP::TextWrapped("%s", page.description.c_str());
                    ImGuiMCP::Separator();
                }
                if (editModeEnabled && !definition.lockEditMode)
                {
                    DrawLayoutEditor(a_menu, a_pageIndex, page);
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0.0f, ImGuiMCP::GetFrameHeight()));
                }
                DrawPageModules(definition, page.modules, std::to_string(a_pageIndex));
                ImGuiMCP::EndTabItem();
            };

            std::size_t customPageIndex = 0;
            for (std::size_t position = 0; position <= definition.pages.size(); ++position)
            {
                if (position == definition.profilePage.order)
                {
                    drawProfilePage();
                }
                else
                {
                    drawCustomPage(customPageIndex++);
                }
            }
            ImGuiMCP::EndTabBar();
        }
        void RenderProfilePage(const std::size_t a_index)
        {
            if (!menuFrameworkOpen.load(std::memory_order_acquire))
            {
                return;
            }

            SKSEMenuSettings::ReloadIfChanged();
            if (a_index < profileMenus.size()) ObserveMenuPage(profileMenus[a_index].definition.profile);
            const ButtonFeedbackStyle buttonFeedback;
            DisableFrameworkEffectsForLuma();
            ReloadProfileMenusIfChanged();
            if (a_index >= registeredProfilePaths.size())
            {
                DrawDisplayText("profilePageUnavailable");
                return;
            }

            const auto& path = registeredProfilePaths[a_index];
            const auto menu = std::ranges::find(profileMenus, path, &LoadedMenu::path);
            if (menu == profileMenus.end())
            {
                DrawDisplayText("menuDefinitionUnavailable");
                return;
            }

            ActivateWeatherLockPreference(menu->definition);
            DrawProfileMenu(*menu);
            CommitLightPlacerAfterSliderRelease();
            if (SKSEMenuSettings::GetStatusLocation() == SKSEMenuSettings::StatusLocation::bottom)
            {
                DrawStatusMessage(statusMessage);
            }
        }

        void __stdcall RenderSettingsPage()
        {
            SKSEMenuSettings::ReloadIfChanged();
            ObserveMenuPage("Settings");
            const ButtonFeedbackStyle buttonFeedback;
            const auto pageTopSpacing = SKSEMenuSettings::GetPageTopSpacing();
            if (pageTopSpacing > 0.0f) ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0.0f, pageTopSpacing));
            if (SKSEMenuSettings::GetStatusLocation() == SKSEMenuSettings::StatusLocation::top)
            {
                DrawStatusMessage(settingsStatusMessage, "TuningSettingsTopStatus");
            }
            auto tuningMenuEnabled = TuningSettings::IsTuningMenuConfigured();
            if (ImGuiMCP::Checkbox("Enable Tuning Menu##TuningSettings", &tuningMenuEnabled))
            {
                if (TuningSettings::SetTuningMenuConfigured(tuningMenuEnabled))
                {
                    settingsStatusMessage = StatusText(tuningMenuEnabled ? "tuningMenuEnabled" : "tuningMenuDisabled");
                }
                else
                {
                    settingsStatusMessage = StatusText("tuningMenuSaveFailure");
                }
            }
            ImGuiMCP::SameLine();
            ImGuiMCP::TextDisabled("(Requires Restart)");

            auto detailedLogging = TuningSettings::IsDetailedLoggingConfigured();
            if (ImGuiMCP::Checkbox("Detailed Logging##TuningSettings", &detailedLogging))
            {
                if (TuningSettings::SetDetailedLoggingConfigured(detailedLogging))
                {
                    settingsStatusMessage = StatusText(
                        detailedLogging ? "detailedLoggingEnabled" : "detailedLoggingDisabled");
                }
                else
                {
                    settingsStatusMessage = StatusText("detailedLoggingSaveFailure");
                }
            }

            const auto editModeLabel = SKSEMenuSettings::Label("editMode", "Edit Mode") + "##TuningSettings";
            if (ImGuiMCP::Checkbox(editModeLabel.c_str(), &editModeEnabled))
            {
                if (editModeEnabled)
                {
                    settingsStatusMessage = StatusText("editModeEnabled");
                }
                else
                {
                    DiscardAllLayoutEditSessions();
                    settingsStatusMessage = StatusText("editModeDisabled");
                }
            }
            if (editModeEnabled) DrawAllLayoutEditActions();

            constexpr auto deleteUserSettingsPopup = "Delete User Settings?##TuningSettings";
            {
                const ButtonColorStyle color(
                    SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                const auto deleteUserSettingsLabel =
                    SKSEMenuSettings::Label("deleteUserSettings", "Delete User Settings");
                if (ImGuiMCP::Button((deleteUserSettingsLabel + "##TuningSettings").c_str()))
                {
                    ImGuiMCP::OpenPopup(deleteUserSettingsPopup);
                }
            }
            if (ImGuiMCP::BeginPopupModal(
                    deleteUserSettingsPopup,
                    nullptr,
                    ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize))
            {
                const auto confirmation = DisplayText("deleteUserSettingsConfirmation");
                ImGuiMCP::TextWrapped("%s", confirmation.c_str());
                {
                    const ButtonColorStyle color(
                        SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::destructive));
                    const auto confirmLabel =
                        SKSEMenuSettings::Label("confirmDeleteUserSettings", "Delete") +
                        "##ConfirmDeleteUserSettings";
                    if (ImGuiMCP::Button(confirmLabel.c_str()))
                    {
                        settingsStatusMessage = StatusText(
                            DeleteGeneratedUserSettings() ?
                                "userSettingsDeleted" :
                                "userSettingsDeleteFailure");
                        ImGuiMCP::CloseCurrentPopup();
                    }
                }
                SameActionLine();
                const auto cancelLabel =
                    SKSEMenuSettings::Label("cancelDeleteUserSettings", "Cancel") +
                    "##CancelDeleteUserSettings";
                if (ImGuiMCP::Button(cancelLabel.c_str())) ImGuiMCP::CloseCurrentPopup();
                ImGuiMCP::EndPopup();
            }

            DrawHeader(SKSEMenuSettings::Label("createProfileSection", "Create Profile"));
            std::vector<const TuningUtil::Profile*> copySources;
            for (const auto& registeredPath : registeredProfilePaths)
            {
                const auto menu = std::ranges::find(profileMenus, registeredPath, &LoadedMenu::path);
                if (menu == profileMenus.end()) continue;
                const auto profile = std::ranges::find_if(TuningUtil::GetProfiles(), [&](const TuningUtil::Profile& a_profile)
                    { return Config::IEquals(a_profile.name, menu->definition.profile); });
                if (profile != TuningUtil::GetProfiles().end()) copySources.push_back(std::addressof(*profile));
            }
            std::ranges::sort(copySources, [](const auto* a_left, const auto* a_right)
                {
                const auto left = Lowercase(a_left->name);
                const auto right = Lowercase(a_right->name);
                return left != right ? left < right : a_left->name < a_right->name; });
            if (!profileCopySource.empty() && !std::ranges::any_of(copySources, [&](const auto* a_profile)
                                                  { return Config::IEquals(a_profile->name, profileCopySource); }))
            {
                profileCopySource.clear();
            }
            const auto blankProfileLabel = SKSEMenuSettings::Label("blankProfile", "Blank Profile");
            const auto copyProfileLabel = SKSEMenuSettings::Label("copyExistingProfile", "Copy Existing Profile") +
                                          "##TuningSettings";
            const auto copyPreview = profileCopySource.empty() ? blankProfileLabel : profileCopySource;
            if (ImGuiMCP::BeginCombo(
                    copyProfileLabel.c_str(),
                    copyPreview.c_str(),
                    ImGuiMCP::ImGuiComboFlags_HeightLargest))
            {
                if (ImGuiMCP::Selectable(blankProfileLabel.c_str(), profileCopySource.empty()))
                {
                    profileCopySource.clear();
                }
                for (const auto* profile : copySources)
                {
                    const auto selected = Config::IEquals(profile->name, profileCopySource);
                    const auto label = profile->name + "##CopyProfile" + profile->directory.string();
                    if (ImGuiMCP::Selectable(label.c_str(), selected)) profileCopySource = profile->name;
                    if (selected) ImGuiMCP::SetItemDefaultFocus();
                }
                ImGuiMCP::EndCombo();
            }
            ImGuiMCP::SetNextItemWidth(320.0f);
            const auto profileNameHint = DisplayText("newProfileNameHint");
            ImGuiMCP::InputTextWithHint(
                "Profile Name##TuningSettings",
                profileNameHint.c_str(),
                newProfileName.data(),
                newProfileName.size());
            const ButtonColorStyle saveColor(SKSEMenuSettings::GetButtonColor(SKSEMenuSettings::ButtonKind::save));
            const auto createProfileLabel = SKSEMenuSettings::Label("createProfile", "Create New Profile");
            if (ImGuiMCP::Button((createProfileLabel + "##TuningSettings").c_str()))
            {
                const auto profileName = InputText(newProfileName);
                std::string error;
                const auto source = std::ranges::find_if(copySources, [&](const auto* a_profile)
                    { return Config::IEquals(a_profile->name, profileCopySource); });
                const auto sourcePath = source != copySources.end() ? (*source)->directory : std::filesystem::path{};
                if (SliderCreator::CreateProfile(kProfileRoot, profileName, error, sourcePath))
                {
                    logger::info("[Tuning Menu] profile={} | status=created | path={}", profileName, (kProfileRoot / profileName).string());
                    newProfileName.fill('\0');
                    profileCopySource.clear();
                    settingsStatusMessage = StatusText("profileCreated", { { "profile", profileName } });
                }
                else
                {
                    logger::warn("[Tuning Menu] profile={} | create failed | {}", profileName, error);
                    settingsStatusMessage = StatusText("profileCreateFailure", { { "reason", SliderCreatorErrorText(error) } });
                }
            }

            if (SKSEMenuSettings::GetStatusLocation() == SKSEMenuSettings::StatusLocation::bottom)
            {
                DrawStatusMessage(settingsStatusMessage);
            }
        }

        template <std::size_t Index>
        void __stdcall RenderProfilePageSlot()
        {
            RenderProfilePage(Index);
        }

        template <std::size_t... Indices>
        constexpr auto MakeProfileRenderers(std::index_sequence<Indices...>)
        {
            return std::array<SKSEMenuFramework::Model::RenderFunction, sizeof...(Indices)>{
                &RenderProfilePageSlot<Indices>...
            };
        }

        constexpr std::size_t kMaximumProfilePages = 64;
        constexpr auto kProfileRenderers = MakeProfileRenderers(std::make_index_sequence<kMaximumProfilePages>{});
    }  // namespace

    void Register()
    {
        if (registered)
        {
            return;
        }
        if (!SKSEMenuFramework::IsInstalled())
        {
            logger::info("[Tuning Menu] status=disabled | SKSE Menu Framework missing");
            return;
        }

        const auto frameworkVersion = SKSEMenuFramework::GetMenuFrameworkVersion();
        if (frameworkVersion <= 0.0f)
        {
            logger::warn("[Tuning Menu] registration failed | SKSE Menu Framework API unavailable");
            return;
        }

        SKSEMenuSettings::Initialize();
        SKSEMenuFramework::SetSection("Luma");

        if (!TuningSettings::IsTuningMenuEnabledForSession())
        {
            SKSEMenuFramework::AddSectionItem(SKSEMenuSettings::SettingsProfileLabel(false), RenderSettingsPage);
            registered = true;
            logger::info(
                "[Tuning Menu] registered | framework={} | settingsPage=true | profilePages=0 | tuningMenu=false",
                frameworkVersion);
            return;
        }

        ReloadProfileMenus(true);
        menuEffectOverride.freezeTimeEnabled = ReadFrameworkBoolean("FreezeTimeOnMenu", true);
        menuEffectOverride.backgroundBlurEnabled = ReadFrameworkBoolean("BlurBackgroundOnMenu", true);
        menuFrameworkEvent = SKSEMenuFramework::AddEvent(HandleMenuFrameworkEvent, 0.0f);
        logger::info(
            "[Tuning Menu] effects | freeze={} | blur={}",
            menuEffectOverride.freezeTimeEnabled,
            menuEffectOverride.backgroundBlurEnabled);
        const auto pageCount = std::min(profileMenus.size(), kMaximumProfilePages);
        registeredProfilePaths.reserve(pageCount);
        if (SKSEMenuSettings::SettingsProfileFirst())
        {
            SKSEMenuFramework::AddSectionItem(SKSEMenuSettings::SettingsProfileLabel(false), RenderSettingsPage);
        }
        for (std::size_t index = 0; index < pageCount; ++index)
        {
            registeredProfilePaths.push_back(profileMenus[index].path);
            SKSEMenuFramework::AddSectionItem(profileMenus[index].definition.title, kProfileRenderers[index]);
            logger::info(
                "[Tuning Menu] page={} | source={}",
                profileMenus[index].definition.title,
                profileMenus[index].path.string());
        }
        if (!SKSEMenuSettings::SettingsProfileFirst())
        {
            SKSEMenuFramework::AddSectionItem(SKSEMenuSettings::SettingsProfileLabel(true), RenderSettingsPage);
        }
        if (profileMenus.size() > kMaximumProfilePages)
        {
            logger::warn(
                "[Tuning Menu] profile pages={}/{} | excess ignored",
                profileMenus.size(),
                kMaximumProfilePages);
        }
        registered = true;
        logger::info(
            "[Tuning Menu] registered | framework={} | settingsPage=true | profilePages={}",
            frameworkVersion,
            pageCount);
    }
}  // namespace MPL::TuningMenu
