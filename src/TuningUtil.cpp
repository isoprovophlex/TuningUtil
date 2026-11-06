#include <CSTonemapping.h>
#include <DetailedLogging.h>
#include <FileIO.h>
#include <Config/Common.h>
#include <ImageSpacePatcher.h>
#include <JsonOverlay.h>
#include <JsonFormatting.h>
#include <LightingPatcher.h>
#include <ObjectLightingPatcher.h>
#include <PresetCatalog.h>
#include <ProfileSetup.h>
#include <ProfileParsing.h>
#include <ProfileSettings.h>
#include <SliderCreator.h>
#include <TuningSettings.h>
#include <SliderSettingCatalog.h>
#include <TuningUtil.h>
#include <UserSettings.h>
#include <WeatherPatcher.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <ranges>
#include <unordered_map>
#include <yyjson.h>

namespace MPL::TuningUtil
{
    namespace
    {
        using namespace ProfileParsing;
        const std::filesystem::path kProfileRoot{ "./Data/Luma/Tuning" };
        const std::filesystem::path kUserRoot{ "./Data/SKSE/Plugins/Luma" };
        const std::filesystem::path kGlobalDefaultsPath = kProfileRoot / "defaultSettings.json";
        constexpr std::string_view kProfileDefaultsFile = "profileSettings.json";
        constexpr std::string_view kMenuDefinitionFile = "skseMenu.json";
        constexpr std::string_view kDefaultAmbientAnchorWeather = "SkyrimClear";

        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const { yyjson_doc_free(a_document); }
        };

        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;

        bool discoveryInitialized = false;
        bool pluginDependencyFilterReady = false;
        bool runtimeStateReleased = false;
        std::uint64_t settingsRevision = 0;
        std::uint64_t sliderBindingsRevision = 0;
        std::vector<Profile> profiles;
        std::vector<std::filesystem::path> pluginFilteredProfileDirectories;
        struct PreparedProfileStack
        {
            std::optional<std::string> defaults;
            std::optional<std::string> presets;
            std::optional<std::string> user;
            std::string adjustments;
        };
        struct CachedSettings
        {
            Settings settings;
            std::string localDefaults{ "{}" };
            std::string presetDefaults{ "{}" };
            std::string explicitUserSettings{ "{}" };
            std::optional<std::string> presetPreviewUserLayer;
            std::optional<PreparedProfileStack> preparedStack;
        };
        std::unordered_map<std::string, CachedSettings> settingsCache;
        std::optional<std::string> globalDefaultsCache;
        struct SettingOwnershipLayers
        {
            std::array<std::vector<std::string>, 3> paths;
        };
        std::unordered_map<std::string, SettingOwnershipLayers> settingOwnershipCache;
        std::unordered_map<std::string, bool> startupSettingTargetOverlapCache;
        bool startupSettingTargetOverlapsCaptured = false;
        std::uint64_t settingOwnershipRevision = std::numeric_limits<std::uint64_t>::max();

        std::string SerializeSettings(const Settings& a_settings)
        {
            return rfl::json::write<rfl::NoOptionals>(
                a_settings,
                rfl::json::pretty);
        }



        std::string ProfileName(std::string a_name)
        {
            a_name = std::filesystem::path(std::move(a_name)).filename().string();
            if (a_name.empty() || std::ranges::all_of(a_name, [](const unsigned char a_character)
                                     { return std::isspace(a_character) != 0; }))
            {
                return {};
            }
            const auto extension = std::filesystem::path(a_name).extension().string();
            if (Config::IEquals(extension, ".esp") || Config::IEquals(extension, ".esm") || Config::IEquals(extension, ".esl"))
            {
                a_name = std::filesystem::path(a_name).stem().string();
            }
            return a_name;
        }

        std::optional<std::string> ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            std::string text(std::istreambuf_iterator<char>(file), {});
            if (file.bad()) return std::nullopt;
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom))
            {
                text.erase(0, bom.size());
            }
            return text;
        }

        Document Parse(const std::string_view a_json)
        {
            return Document(yyjson_read(
                const_cast<char*>(a_json.data()),
                a_json.size(),
                YYJSON_READ_NOFLAG));
        }

        std::optional<std::string> UserSettingsValuesOnly(
            const std::string_view a_json,
            std::string& a_error)
        {
            return UserSettings::ValuesOnly(a_json, a_error);
        }

        std::string PresetSelectionsText(const PresetSelections& a_selections)
        {
            return UserSettings::PresetSelectionsText(a_selections);
        }

        template <class Links, class Field>
        std::optional<Links> CreatorCustomLinks(
            const std::optional<SliderCreator::CustomLinks>& a_configured,
            const std::span<const std::string_view> a_fields,
            Field&& a_field)
        {
            if (!a_configured) return std::nullopt;
            Links result;
            for (const auto field : a_fields)
                if (auto* value = a_field(result, field)) value->reset();
            for (const auto& [targetName, link] : *a_configured)
                if (auto* target = a_field(result, targetName)) *target = link;
            return result;
        }

        std::optional<WeatherPatcher::WeatherLinks> CreatorWeatherLinks(
            const std::optional<SliderCreator::CustomLinks>& a_configured)
        {
            static constexpr std::array fields{
                std::string_view{ "ambient" }, std::string_view{ "sunlight" },
                std::string_view{ "effectLighting" }, std::string_view{ "fogFar" },
                std::string_view{ "fogNear" }, std::string_view{ "water" },
                std::string_view{ "skyStatics" }, std::string_view{ "skyUpper" },
                std::string_view{ "skyLower" }, std::string_view{ "horizon" },
                std::string_view{ "sun" }, std::string_view{ "sunGlare" },
                std::string_view{ "moonGlare" }, std::string_view{ "stars" },
                std::string_view{ "cloudLayers" }, std::string_view{ "volumetricLighting" },
            };
            return CreatorCustomLinks<WeatherPatcher::WeatherLinks>(
                a_configured,
                fields,
                WeatherCustomLinkField);
        }

        std::optional<LightingPatcher::LightingLinks> CreatorLightingLinks(
            const std::optional<SliderCreator::CustomLinks>& a_configured)
        {
            static constexpr std::array fields{
                std::string_view{ "ambient" }, std::string_view{ "directional" },
                std::string_view{ "ambientColors" }, std::string_view{ "fogFar" },
                std::string_view{ "fogNear" },
            };
            return CreatorCustomLinks<LightingPatcher::LightingLinks>(
                a_configured,
                fields,
                LightingCustomLinkField);
        }

        std::optional<WeatherPatcher::AmbientHueScales> CreatorHueScales(
            const std::optional<SliderCreator::HueScales>& a_configured)
        {
            if (!a_configured) return std::nullopt;
            const auto& scales = *a_configured;
            return WeatherPatcher::AmbientHueScales{
                scales.red, scales.orange, scales.yellow, scales.green,
                scales.teal, scales.blue, scales.magenta,
            };
        }

        std::optional<ProfileLayout> ReadProfileLayout(const std::filesystem::path& a_directory, std::string& a_error,
            const bool a_allowMissing = false)
        {
            a_error.clear();
            const auto sourcePath = a_directory / kMenuDefinitionFile;
            const auto path = SliderCreator::ActiveLayoutPath(sourcePath);
            std::error_code fileError;
            if (a_allowMissing && !std::filesystem::exists(path, fileError) && !fileError)
                return ProfileLayout{};
            auto text = ReadText(path);
            if (text)
            {
                auto layout = ProfileParsing::ParseLayout(*text, path, a_error);
                if (layout && path != sourcePath)
                {
                    const auto sourceText = ReadText(sourcePath);
                    const auto source = sourceText ? Parse(*sourceText) : Document{};
                    auto name = ReadProfileName(source ? yyjson_doc_get_root(source.get()) : nullptr, sourcePath, a_error);
                    if (!name) return std::nullopt;
                    layout->profile = std::move(*name);
                }
                return layout;
            }
            a_error = std::format("The profile layout could not be read: {}", path.string());
            return std::nullopt;
        }

        std::optional<std::string> StringMember(
            const std::string_view a_json,
            const std::string_view a_name)
        {
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            auto* value = yyjson_is_obj(root) ? yyjson_obj_getn(root, a_name.data(), a_name.size()) : nullptr;
            return yyjson_is_str(value) ?
                       std::optional<std::string>{ std::string(yyjson_get_str(value), yyjson_get_len(value)) } :
                       std::nullopt;
        }

        std::optional<int> IntegerMember(
            const std::string_view a_json,
            const std::string_view a_name)
        {
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            auto* value = yyjson_is_obj(root) ? yyjson_obj_getn(root, a_name.data(), a_name.size()) : nullptr;
            if (!yyjson_is_int(value) && !yyjson_is_uint(value))
            {
                return std::nullopt;
            }
            const auto priority = yyjson_get_sint(value);
            return priority >= std::numeric_limits<int>::min() && priority <= std::numeric_limits<int>::max() ?
                       std::optional<int>{ static_cast<int>(priority) } :
                       std::nullopt;
        }

        std::optional<bool> BooleanMember(
            const std::string_view a_json,
            const std::string_view a_name)
        {
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            auto* value = yyjson_is_obj(root) ? yyjson_obj_getn(root, a_name.data(), a_name.size()) : nullptr;
            return yyjson_is_bool(value) ?
                       std::optional<bool>{ yyjson_get_bool(value) } :
                       std::nullopt;
        }

        bool AnyPluginLoaded(const std::span<const std::string> a_plugins)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler && std::ranges::any_of(a_plugins, [&](const auto& a_plugin)
            {
                return dataHandler->LookupLoadedModByName(a_plugin) ||
                       dataHandler->LookupLoadedLightModByName(a_plugin);
            });
        }

        bool PluginDependenciesSatisfied(const std::span<const std::string> a_dependencies)
        {
            return a_dependencies.empty() || AnyPluginLoaded(a_dependencies);
        }

        std::vector<std::string> SettingRoots(const std::string_view a_json)
        {
            std::vector<std::string> roots;
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (!yyjson_is_obj(root))
            {
                return roots;
            }
            yyjson_obj_iter iterator = yyjson_obj_iter_with(root);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                roots.emplace_back(yyjson_get_str(key), yyjson_get_len(key));
            }
            return roots;
        }

        void AddSettingPath(std::vector<std::string>& a_paths, const std::string_view a_path)
        {
            if (!a_path.empty() && !std::ranges::contains(a_paths, a_path))
            {
                a_paths.emplace_back(a_path);
            }
        }

        void AddExpandedSettingPaths(std::vector<std::string>& a_paths, const std::string_view a_setting)
        {
            if (a_setting == "brightness")
            {
                AddSettingPath(a_paths, "brightnessMultiplier");
                AddSettingPath(a_paths, "volumetricLightingIntensityMultiplier");
            }
            else if (a_setting == "saturation") AddSettingPath(a_paths, "saturationMultiplier");
            else if (a_setting == "ambientCompression")
            {
                AddSettingPath(a_paths, "brightnessMultiplier.ambient");
                AddSettingPath(a_paths, "withinWeatherCompression.ambient");
                AddSettingPath(a_paths, "betweenWeatherCompression.ambient");
            }
            else if (a_setting == "sunlightCompression")
            {
                AddSettingPath(a_paths, "brightnessMultiplier.sunlight");
                AddSettingPath(a_paths, "withinWeatherCompression.sunlight");
                AddSettingPath(a_paths, "betweenWeatherCompression.sunlight");
            }
            else if (a_setting == "lightBrightness")
            {
                AddSettingPath(a_paths, "lightBrightnessMultiplier");
                AddSettingPath(a_paths, "lightFogPowerMultiplier");
                AddSettingPath(a_paths, "lightFogMaxMultiplier");
            }
            else AddSettingPath(a_paths, a_setting);
        }

        const Profile* FindProfile(const std::string& a_name)
        {
            const auto name = ProfileName(a_name);
            const auto& discovered = GetProfiles();
            const auto match = std::ranges::find_if(discovered, [&](const Profile& a_profile)
                { return Config::IEquals(a_profile.name, name); });
            return match != discovered.end() ? std::addressof(*match) : nullptr;
        }

        std::filesystem::path UserSettingsPath(const Profile& a_profile)
        {
            return kUserRoot / a_profile.directory.filename() / "userSettings.json";
        }

        std::filesystem::path ProfileDefaultsPath(const Profile& a_profile)
        {
            return a_profile.directory / kProfileDefaultsFile;
        }

        std::optional<std::string> ProfileDefaultsText(const Profile& a_profile, std::string& a_error)
        {
            auto defaults = ProfileSettings::Load(a_profile.directory, a_error);
            return defaults ? std::optional{ std::move(defaults->text) } : std::nullopt;
        }



        std::string SerializeProfileSettings(const Profile& a_profile, const Settings& a_settings)
        {
            std::string error;
            const auto raw = SerializeSettings(a_settings);
            return SliderStorage::Store(raw, a_profile.sliderBindings, false, error).value_or(raw);
        }

        std::optional<Settings> ParseProfileSettings(const std::string& a_json, const Profile& a_profile)
        {
            std::string error;
            const auto expanded = SliderStorage::Materialize(a_json, a_profile.sliderBindings, error);
            return expanded ? ParseSettings(*expanded, a_profile.directory / "slider values") : std::nullopt;
        }

        const std::string& GlobalDefaultsText()
        {
            if (globalDefaultsCache)
            {
                return *globalDefaultsCache;
            }
            if (const auto text = ReadText(kGlobalDefaultsPath); text && ParseSettings(*text, kGlobalDefaultsPath))
            {
                globalDefaultsCache = *text;
                return *globalDefaultsCache;
            }

            logger::warn(
                "[TuningUtil] global defaults invalid | source={} | fallback=compiled",
                kGlobalDefaultsPath.string());
            globalDefaultsCache = SerializeSettings(Settings{});
            return *globalDefaultsCache;
        }

        std::optional<std::string> LocalDefaultsText(const Profile& a_profile, std::string& a_error)
        {
            const auto profileDefaults = ProfileDefaultsText(a_profile, a_error);
            const auto local = profileDefaults ?
                                   JsonOverlay::Overlay(GlobalDefaultsText(), *profileDefaults, a_error) :
                                   std::nullopt;
            const auto savedLayout = ReadText(a_profile.directory / "skseMenu.json");
            const auto savedBindings = savedLayout ? SliderStorage::ReadLayout(*savedLayout, a_error) : a_profile.sliderBindings;
            const auto remapped = local ? SliderStorage::Remap(*local, savedBindings, a_profile.sliderBindings, a_error) : std::nullopt;
            return remapped ? SliderStorage::Store(*remapped, a_profile.sliderBindings, true, a_error) : std::nullopt;
        }

        std::optional<std::string> ActivePresetSettingsText(
            const Profile& a_profile,
            std::string& a_error)
        {
            auto profileName = a_profile.name;
            const auto active = WeatherPatcher::GetActivePresetSettings(profileName, a_error);
            const auto schema = active ? LocalDefaultsText(a_profile, a_error) : std::nullopt;
            const auto stored = active ? SliderStorage::Store(*active, a_profile.sliderBindings, false, a_error) : std::nullopt;
            return stored && schema ? JsonOverlay::ProjectLike(*stored, *schema, a_error) : std::nullopt;
        }

        std::optional<std::string> PresetDefaultsText(
            const Profile& a_profile,
            std::string& a_error)
        {
            const auto localDefaults = LocalDefaultsText(a_profile, a_error);
            const auto activePresets = localDefaults ? ActivePresetSettingsText(a_profile, a_error) : std::nullopt;
            const auto defaults = localDefaults && activePresets ?
                                      JsonOverlay::Overlay(*localDefaults, *activePresets, a_error) :
                                      std::nullopt;
            return defaults;
        }

        std::optional<std::string> StoredSettingsText(const Profile&, std::string_view, std::string&);

        std::optional<CachedSettings> LoadStoredSettings(const Profile& a_profile)
        {
            std::string error;
            const auto localDefaults = LocalDefaultsText(a_profile, error);
            if (!localDefaults)
            {
                logger::warn("[TuningUtil] {} defaults compose failed | {}", a_profile.name, error);
                return std::nullopt;
            }
            const auto presetDefaults = PresetDefaultsText(a_profile, error);
            auto defaults = presetDefaults ? ParseProfileSettings(*presetDefaults, a_profile) : std::nullopt;
            if (!defaults)
            {
                logger::warn("[TuningUtil] {} preset compose failed | {}", a_profile.name, error);
                return std::nullopt;
            }

            auto settings = *defaults;
            const auto stored = StoredSettingsText(a_profile, *presetDefaults, error);
            std::string explicitUserSettings{ "{}" };
            if (stored && std::filesystem::is_regular_file(UserSettingsPath(a_profile)))
            {
                if (auto parsed = ParseProfileSettings(*stored, a_profile))
                {
                    settings = std::move(*parsed);
                    const auto userText = ReadText(UserSettingsPath(a_profile)).value_or("{}");
                    const auto converted = SliderStorage::Store(userText, a_profile.sliderBindings, false, error);
                    explicitUserSettings = UserSettingsValuesOnly(converted.value_or(userText), error).value_or("{}");
                }
            }
            else if (!stored)
            {
                logger::warn("[TuningUtil] user settings merge failed | source={} | {}", UserSettingsPath(a_profile).string(), error);
            }
            return CachedSettings{
                std::move(settings),
                *localDefaults,
                *presetDefaults,
                std::move(explicitUserSettings) };
        }

        bool WriteTextAtomically(
            const std::filesystem::path&,
            std::string_view,
            std::string&);

        std::optional<UserSettings::SanitizeResult> SanitizeUserSettingsText(
            const Profile& a_profile,
            const std::string_view a_text,
            std::string& a_error)
        {
            const auto settingsSchema = LocalDefaultsText(a_profile, a_error);
            if (!settingsSchema) return std::nullopt;

            std::optional<PresetCatalog::Catalog> presetCatalog;
            const auto presetCatalogPath = a_profile.directory / PresetCatalog::kFileName;
            std::error_code fileError;
            const auto hasPresetCatalog = std::filesystem::is_regular_file(presetCatalogPath, fileError);
            if (fileError)
            {
                logger::warn(
                    "[TuningUtil] preset catalog sanitation unavailable | profile={} | path={} | {}",
                    a_profile.name,
                    presetCatalogPath.string(),
                    fileError.message());
            }
            else if (hasPresetCatalog)
            {
                std::string presetError;
                presetCatalog = PresetCatalog::Read(presetCatalogPath, presetError);
                if (!presetCatalog)
                {
                    logger::warn(
                        "[TuningUtil] preset selection sanitation skipped | profile={} | {}",
                        a_profile.name,
                        presetError);
                }
            }
            else
            {
                presetCatalog.emplace();
            }

            return UserSettings::Sanitize(
                a_text,
                *settingsSchema,
                presetCatalog ? std::addressof(*presetCatalog) : nullptr,
                a_error);
        }

        void SanitizeStoredUserSettings(const Profile& a_profile)
        {
            const auto path = UserSettingsPath(a_profile);
            std::error_code fileError;
            if (!std::filesystem::is_regular_file(path, fileError))
            {
                if (fileError)
                {
                    logger::warn(
                        "[TuningUtil] user settings sanitation skipped | profile={} | path={} | {}",
                        a_profile.name,
                        path.string(),
                        fileError.message());
                }
                return;
            }

            const auto text = ReadText(path);
            std::string error;
            const auto sanitized = text ?
                                       SanitizeUserSettingsText(a_profile, *text, error) :
                                       std::nullopt;
            if (!sanitized)
            {
                logger::warn(
                    "[TuningUtil] user settings sanitation failed | profile={} | path={} | {}",
                    a_profile.name,
                    path.string(),
                    error.empty() ? "The user settings file could not be read." : error);
                return;
            }
            if (!sanitized->Changed()) return;

            auto backup = path;
            backup += ".before-slider-values";
            if (!std::filesystem::exists(backup, fileError))
            {
                std::filesystem::copy_file(path, backup, std::filesystem::copy_options::none, fileError);
                if (fileError)
                {
                    logger::warn("[TuningUtil] slider storage backup failed | {}", fileError.message());
                    return;
                }
            }
            const auto output = JsonFormatting::Pretty(sanitized->text);
            if (!WriteTextAtomically(path, output, error))
            {
                logger::warn(
                    "[TuningUtil] user settings sanitation save failed | profile={} | path={} | {}",
                    a_profile.name,
                    path.string(),
                    error);
                return;
            }
        }

        std::vector<std::string> ExpandSettingScopes(const std::span<const std::string> a_scopes)
        {
            std::vector<std::string> paths;
            for (const auto& scope : a_scopes)
            {
                AddExpandedSettingPaths(paths, scope);
            }
            return paths;
        }

        std::optional<std::string> CombineDeclaredSettings(
            const std::string_view a_source,
            const std::string_view a_defaults,
            std::string& a_error)
        {
            return JsonOverlay::ProjectLike(a_source, a_defaults, a_error);
        }

        std::optional<std::string> StoredSettingsText(
            const Profile& a_profile,
            const std::string_view a_defaults,
            std::string& a_error)
        {
            const auto user = ReadText(UserSettingsPath(a_profile));
            if (!user)
            {
                return std::string(a_defaults);
            }
            const auto converted = SliderStorage::Store(*user, a_profile.sliderBindings, false, a_error);
            const auto values = converted ? UserSettingsValuesOnly(*converted, a_error) : std::nullopt;
            return values ? JsonOverlay::Merge(a_defaults, *values, a_error) : std::nullopt;
        }

        bool ApplySettingsPatch(
            const Profile& a_profile,
            std::string& a_profileName,
            const std::string_view a_patch,
            const std::string_view a_trigger,
            const std::source_location a_source = std::source_location::current())
        {
            const auto patch = Parse(a_patch);
            auto* patchRoot = patch ? yyjson_doc_get_root(patch.get()) : nullptr;
            if (!yyjson_is_obj(patchRoot))
            {
                return false;
            }
            if (yyjson_obj_size(patchRoot) == 0)
            {
                return true;
            }

            std::string error;
            const auto current = SerializeProfileSettings(a_profile, GetSettings(a_profileName));
            const auto converted = SliderStorage::Store(a_patch, a_profile.sliderBindings, false, error);
            const auto overlaid = converted ? JsonOverlay::Overlay(current, *converted, error) : std::nullopt;
            auto parsed = overlaid ? ParseProfileSettings(*overlaid, a_profile) : std::nullopt;
            if (!parsed)
            {
                logger::warn("[TuningUtil] {} settings patch failed | {}", a_profileName, error);
                return false;
            }
            GetSettings(a_profileName) = std::move(*parsed);
            ApplySettings(std::format("{} | profile={}", a_trigger, a_profileName), true, a_source);
            return true;
        }

        bool WriteUserSettings(
            const Profile& a_profile,
            const std::string_view a_settings,
            const std::optional<PresetSelections>& a_presetSelections = std::nullopt)
        {
            const auto path = UserSettingsPath(a_profile);
            std::string error;
            const auto values = UserSettingsValuesOnly(a_settings, error);
            auto selections = a_presetSelections;
            if (!selections)
            {
                const auto defaults = ProfileDefaultsText(a_profile, error);
                const auto existing = ReadText(path);
                selections = defaults ? UserSettings::ResolvePresetSelections(
                    *defaults, existing ? std::optional<std::string_view>{ *existing } : std::nullopt, error) : std::nullopt;
            }
            const auto output = values && selections ?
                                    JsonOverlay::Overlay(*values, PresetSelectionsText(*selections), error) :
                                    std::nullopt;
            if (!output)
            {
                logger::warn("[TuningUtil] user settings prepare failed | path={} | {}", path.string(), error);
                return false;
            }
            std::error_code fileError;
            std::filesystem::create_directories(path.parent_path(), fileError);
            if (fileError)
            {
                logger::warn("[TuningUtil] user directory create failed | path={} | {}", path.parent_path().string(), fileError.message());
                return false;
            }
            const auto sanitized = SanitizeUserSettingsText(a_profile, *output, error);
            if (!sanitized)
            {
                logger::warn("[TuningUtil] user settings sanitation failed | path={} | {}", path.string(), error);
                return false;
            }
            if (!WriteTextAtomically(path, JsonFormatting::Pretty(sanitized->text), error))
            {
                logger::warn("[TuningUtil] user settings save failed | path={} | {}", path.string(), error);
                return false;
            }
            if (const auto cached = settingsCache.find(Lowercase(a_profile.name)); cached != settingsCache.end())
                cached->second.preparedStack.reset();
            return true;
        }

        bool WriteTextAtomically(
            const std::filesystem::path& a_path,
            const std::string_view a_text,
            std::string& a_error)
        {
            return FileIO::WriteAtomically(a_path, std::string(a_text) + '\n', a_error);
        }

        bool WriteSettings(
            const Profile& a_profile,
            const std::string& a_profileName,
            const Settings& a_settings)
        {
            std::string error;
            const auto defaultText = PresetDefaultsText(a_profile, error);
            if (!defaultText)
            {
                return false;
            }
            if (!ParseSettings(*defaultText, ProfileDefaultsPath(a_profile)))
            {
                logger::warn("[TuningUtil] {} save rejected | defaults unreadable", a_profileName);
                return false;
            }
            const auto current = SerializeProfileSettings(a_profile, a_settings);
            const auto declared = CombineDeclaredSettings(current, *defaultText, error);
            if (!declared)
            {
                logger::warn("[TuningUtil] {} save failed | declared settings | {}", a_profileName, error);
                return false;
            }
            const auto difference = JsonOverlay::Difference(*declared, *defaultText, error);
            const auto userDifference = difference ?
                                            UserSettingsValuesOnly(*difference, error) :
                                            std::nullopt;
            if (!userDifference)
            {
                logger::warn("[TuningUtil] {} save failed | sparse settings | {}", a_profileName, error);
                return false;
            }
            auto profileName = a_profileName;
            const auto activePresets = WeatherPatcher::GetActivePresets(profileName, error);
            if (!error.empty()) return false;
            PresetSelections selections;
            for (const auto& preset : activePresets) selections.insert_or_assign(preset.category, preset.name);
            if (!WriteUserSettings(a_profile, *userDifference, selections))
            {
                return false;
            }
            if (auto cached = settingsCache.find(Lowercase(a_profile.name)); cached != settingsCache.end())
            {
                cached->second.explicitUserSettings = *userDifference;
                cached->second.presetPreviewUserLayer.reset();
            }
            return true;
        }

        bool WriteProfileSetupPatch(
            const Profile& a_profile,
            const std::string_view a_setup,
            std::string& a_error,
            const std::span<const std::string> a_replacePaths)
        {
            const auto path = ProfileDefaultsPath(a_profile);
            const auto existing = ReadText(path).value_or(std::string(ProfileSettings::kNewBaseSettings));
            const auto retained = !a_replacePaths.empty() ?
                                      JsonOverlay::RemovePaths(existing, a_replacePaths, a_error) :
                                      std::optional{ existing };
            const auto updated = retained ?
                                     JsonOverlay::Overlay(*retained, a_setup, a_error) :
                                     std::nullopt;
            if (!updated || !ParseSettings(*updated, path) ||
                !WriteTextAtomically(path, JsonFormatting::Pretty(*updated), a_error))
            {
                if (a_error.empty()) a_error = "The profile setup could not be prepared.";
                return false;
            }

            const auto profile = std::ranges::find_if(profiles, [&](const Profile& a_candidate)
                { return Config::IEquals(a_candidate.name, a_profile.name); });
            if (profile != profiles.end())
                profile->defaultSettingRoots = SettingRoots(ProfileDefaultsText(a_profile, a_error).value_or(*updated));
            if (const auto cached = settingsCache.find(Lowercase(a_profile.name)); cached != settingsCache.end())
                cached->second.preparedStack.reset();
            return true;
        }

        std::optional<std::string> CurrentUserLayer(const Profile& a_profile, std::string& a_error)
        {
            auto profileName = a_profile.name;
            const auto current = SerializeProfileSettings(a_profile, GetSettings(profileName));
            const auto cached = settingsCache.find(Lowercase(a_profile.name));
            if (cached == settingsCache.end())
            {
                a_error = "The profile settings cache is unavailable.";
                return std::nullopt;
            }

            const auto parsedDefaults = ParseSettings(
                cached->second.presetDefaults,
                ProfileDefaultsPath(a_profile));
            if (!parsedDefaults)
            {
                a_error = "The profile preset defaults could not be normalized.";
                return std::nullopt;
            }
            const auto normalizedDefaults = SerializeProfileSettings(a_profile, *parsedDefaults);
            const auto explicitValues = JsonOverlay::ProjectLike(
                current,
                cached->second.explicitUserSettings,
                a_error);
            const auto changedValues = JsonOverlay::Difference(
                current,
                normalizedDefaults,
                a_error);
            return explicitValues && changedValues ?
                       JsonOverlay::Overlay(*explicitValues, *changedValues, a_error) :
                       std::nullopt;
        }

        const PreparedProfileStack& PreparedStack(const Profile& a_profile)
        {
            auto name = a_profile.name;
            const auto& settings = GetSettings(name);
            auto& cached = settingsCache.at(Lowercase(a_profile.name));
            if (!cached.preparedStack)
            {
                std::string error;
                cached.preparedStack = PreparedProfileStack{
                    ProfileDefaultsText(a_profile, error), ActivePresetSettingsText(a_profile, error),
                    CurrentUserLayer(a_profile, error), SerializeSettings(settings) };
                if (!error.empty())
                    logger::warn("[TuningUtil] profile stack preparation failed | profile={} | {}", a_profile.name, error);
            }
            return *cached.preparedStack;
        }

        bool HasAnyRoot(
            const std::span<const std::string> a_declaredRoots,
            const std::span<const std::string_view> a_requestedRoots)
        {
            return std::ranges::any_of(a_requestedRoots, [&](const auto requested)
            {
                return std::ranges::any_of(a_declaredRoots, [&](const auto& declared)
                    { return declared == requested; });
            });
        }

        bool OwnsAnySetting(
            const Profile& a_profile,
            const std::span<const std::string_view> a_requestedRoots)
        {
            for (const auto& binding : a_profile.sliderBindings)
                if (binding.ruleValueMap.empty())
                    for (const auto& target : binding.targets)
                        for (const auto root : a_requestedRoots)
                            if (target.path == root || target.path.starts_with(std::string(root) + ".")) return true;
            if (HasAnyRoot(a_profile.defaultSettingRoots, a_requestedRoots))
            {
                return true;
            }
            std::string error;
            auto profileName = a_profile.name;
            const auto activePresets = WeatherPatcher::GetActivePresetSettings(profileName, error);
            if (activePresets && HasAnyRoot(SettingRoots(*activePresets), a_requestedRoots))
            {
                return true;
            }
            const auto userLayer = CurrentUserLayer(a_profile, error);
            return userLayer && HasAnyRoot(SettingRoots(*userLayer), a_requestedRoots);
        }

        void CollectSettingPaths(
            yyjson_val* a_value,
            const std::string_view a_prefix,
            std::vector<std::string>& a_paths)
        {
            if (!yyjson_is_obj(a_value))
            {
                AddSettingPath(a_paths, a_prefix);
                return;
            }

            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_value);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
                const auto path = a_prefix.empty() ? std::string(name) : std::string(a_prefix) + "." + std::string(name);
                CollectSettingPaths(yyjson_obj_iter_get_val(key), path, a_paths);
            }
        }

        std::vector<std::string> SettingPaths(const std::string_view a_json)
        {
            std::vector<std::string> paths;
            const auto document = Parse(a_json);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (yyjson_is_obj(root))
            {
                CollectSettingPaths(root, {}, paths);
            }
            return paths;
        }

        bool SettingPathsOverlap(const std::string_view a_left, const std::string_view a_right)
        {
            return a_left == a_right ||
                   (a_left.size() > a_right.size() && a_left.starts_with(a_right) && a_left[a_right.size()] == '.') ||
                   (a_right.size() > a_left.size() && a_right.starts_with(a_left) && a_right[a_left.size()] == '.');
        }

        bool IsProfileLocalSettingPath(const std::string_view a_path)
        {
            static constexpr std::array roots{
                std::string_view{ "profile" },
                std::string_view{ "profilePriority" },
                std::string_view{ "PluginDependency" },
                std::string_view{ "DisableProfile" },
                std::string_view{ "exclusiveControl" },
                std::string_view{ "ambientAnchorWeather" },
                std::string_view{ "EnableProfile" },
                std::string_view{ "ShowAdvanced" },
                std::string_view{ "weatherInclusions" },
                std::string_view{ "weatherExclusions" },
                std::string_view{ "weatherPluginOwnership" },
                std::string_view{ "weatherPluginInclusions" },
                std::string_view{ "weatherPluginExclusions" },
                std::string_view{ "lightingTemplateInclusions" },
                std::string_view{ "lightingTemplateExclusions" },
                std::string_view{ "lightingTemplatePluginOwnership" },
                std::string_view{ "lightingTemplatePluginInclusions" },
                std::string_view{ "lightingTemplatePluginExclusions" },
                std::string_view{ "enableTemplateInherit" },
                std::string_view{ "cellExclusions" },
            };
            return std::ranges::any_of(roots, [&](const auto root)
                { return a_path == root || (a_path.starts_with(root) && a_path.size() > root.size() && a_path[root.size()] == '.'); });
        }

        bool ClaimsExclusiveControl(const Settings& a_settings, const ControlDomain a_domain)
        {
            switch (a_domain)
            {
            case ControlDomain::weather:
                return a_settings.exclusiveControl.weather;
            case ControlDomain::exteriorImageSpace:
                return a_settings.exclusiveControl.exteriorImageSpace;
            case ControlDomain::lightingTemplates:
                return a_settings.exclusiveControl.lightingTemplates;
            case ControlDomain::cellLighting:
                return a_settings.exclusiveControl.cellLighting;
            case ControlDomain::lightingImageSpace:
                return a_settings.exclusiveControl.lightingImageSpace;
            }
            return false;
        }

        std::vector<ControlDomain> ControlDomainsForSetting(const std::string_view a_path)
        {
            if (a_path == "exteriorImageSpace" || a_path.starts_with("exteriorImageSpace."))
            {
                return { ControlDomain::exteriorImageSpace };
            }
            if (a_path == "lightImageSpace" || a_path.starts_with("lightImageSpace."))
            {
                return { ControlDomain::lightingImageSpace };
            }
            if (a_path.starts_with("filteredLightingTemplateAdjustments."))
            {
                return { ControlDomain::lightingTemplates };
            }
            if (a_path == "links.lighting" || a_path.starts_with("links.lighting.") ||
                a_path == "normalizeLuminance" || a_path.starts_with("normalizeLuminance.") ||
                a_path == "lightBrightnessMultiplier" || a_path.starts_with("lightBrightnessMultiplier.") ||
                a_path == "lightSaturationMultiplier" || a_path.starts_with("lightSaturationMultiplier.") ||
                a_path == "lightHueShift" || a_path.starts_with("lightHueShift.") ||
                a_path == "lightFogPowerMultiplier" || a_path == "lightFogMaxMultiplier")
            {
                return { ControlDomain::lightingTemplates, ControlDomain::cellLighting };
            }
            if (a_path.starts_with("filteredWeatherAdjustments.") ||
                a_path == "links.weather" || a_path.starts_with("links.weather.") ||
                a_path == "brightnessMultiplier" || a_path.starts_with("brightnessMultiplier.") ||
                a_path == "volumetricLightingIntensityMultiplier" ||
                a_path == "saturationMultiplier" || a_path.starts_with("saturationMultiplier.") ||
                a_path == "hueScales" || a_path.starts_with("hueScales.") ||
                a_path == "hueRanges" || a_path.starts_with("hueRanges.") ||
                a_path == "hueShift" || a_path.starts_with("hueShift.") ||
                a_path == "betweenWeatherCompression" || a_path.starts_with("betweenWeatherCompression.") ||
                a_path == "withinWeatherCompression" || a_path.starts_with("withinWeatherCompression.") ||
                a_path == "compressionAnchor" || a_path.starts_with("compressionAnchor."))
            {
                return { ControlDomain::weather };
            }
            return {};
        }

        enum class SettingFilterDomain
        {
            unfiltered,
            weather,
            filteredWeather,
            lighting,
            filteredLightingTemplate,
            filteredBaseLight,
            filteredObjectLighting,
        };

        SettingFilterDomain FilterDomainForSetting(const std::string_view a_path)
        {
            if (a_path.starts_with("filteredWeatherAdjustments."))
            {
                return SettingFilterDomain::filteredWeather;
            }
            if (a_path.starts_with("filteredLightingTemplateAdjustments."))
            {
                return SettingFilterDomain::filteredLightingTemplate;
            }
            if (a_path.starts_with("filteredBaseLightAdjustments."))
            {
                return SettingFilterDomain::filteredBaseLight;
            }
            if (a_path.starts_with("filteredObjectLightingAdjustments."))
            {
                return SettingFilterDomain::filteredObjectLighting;
            }
            if (a_path == "links.lighting" || a_path.starts_with("links.lighting.") ||
                a_path == "normalizeLuminance" || a_path.starts_with("normalizeLuminance.") ||
                a_path == "lightBrightnessMultiplier" || a_path.starts_with("lightBrightnessMultiplier.") ||
                a_path == "lightSaturationMultiplier" || a_path.starts_with("lightSaturationMultiplier.") ||
                a_path == "lightHueShift" || a_path.starts_with("lightHueShift.") ||
                a_path == "lightFogPowerMultiplier" ||
                a_path == "lightFogMaxMultiplier")
            {
                return SettingFilterDomain::lighting;
            }
            if (a_path == "links.weather" || a_path.starts_with("links.weather.") ||
                a_path == "brightnessMultiplier" || a_path.starts_with("brightnessMultiplier.") ||
                a_path == "volumetricLightingIntensityMultiplier" ||
                a_path == "saturationMultiplier" || a_path.starts_with("saturationMultiplier.") ||
                a_path == "hueScales" || a_path.starts_with("hueScales.") ||
                a_path == "hueRanges" || a_path.starts_with("hueRanges.") ||
                a_path == "hueShift" || a_path.starts_with("hueShift.") ||
                a_path == "betweenWeatherCompression" || a_path.starts_with("betweenWeatherCompression.") ||
                a_path == "withinWeatherCompression" || a_path.starts_with("withinWeatherCompression.") ||
                a_path == "compressionAnchor" || a_path.starts_with("compressionAnchor."))
            {
                return SettingFilterDomain::weather;
            }
            return SettingFilterDomain::unfiltered;
        }

        std::string SettingTargetOverlapKey(
            const std::string& a_leftProfile,
            const std::string& a_rightProfile,
            const std::string_view a_settingPath)
        {
            const auto domain = FilterDomainForSetting(a_settingPath);
            auto leftKey = Lowercase(a_leftProfile);
            auto rightKey = Lowercase(a_rightProfile);
            if (rightKey < leftKey) std::swap(leftKey, rightKey);
            return std::format(
                "{}\x1F{}\x1F{}\x1F{}",
                leftKey,
                rightKey,
                static_cast<int>(domain),
                domain == SettingFilterDomain::filteredWeather ||
                        domain == SettingFilterDomain::filteredLightingTemplate ||
                        domain == SettingFilterDomain::filteredBaseLight ||
                        domain == SettingFilterDomain::filteredObjectLighting ?
                    a_settingPath :
                    std::string_view{});
        }

        bool ComputeProfilesShareSettingTargets(
            const std::string& a_leftProfile,
            const std::string& a_rightProfile,
            const std::string_view a_settingPath)
        {
            const auto domain = FilterDomainForSetting(a_settingPath);
            if (domain == SettingFilterDomain::unfiltered || !RE::TESDataHandler::GetSingleton())
            {
                return true;
            }

            bool sharesTarget = true;
            switch (domain)
            {
            case SettingFilterDomain::weather:
                sharesTarget = WeatherPatcher::ProfilesShareWeatherTarget(a_leftProfile, a_rightProfile);
                break;
            case SettingFilterDomain::filteredWeather:
                sharesTarget = WeatherPatcher::ProfilesShareFilteredWeatherTarget(
                    a_leftProfile,
                    a_rightProfile,
                    a_settingPath.substr(std::string_view("filteredWeatherAdjustments.").size()));
                break;
            case SettingFilterDomain::lighting:
                sharesTarget = LightingPatcher::ProfilesShareLightingTarget(a_leftProfile, a_rightProfile);
                break;
            case SettingFilterDomain::filteredLightingTemplate:
                sharesTarget = LightingPatcher::ProfilesShareFilteredLightingTemplateTarget(
                    a_leftProfile,
                    a_rightProfile,
                    a_settingPath.substr(std::string_view("filteredLightingTemplateAdjustments.").size()));
                break;
            case SettingFilterDomain::filteredBaseLight:
                sharesTarget = LightingPatcher::ProfilesShareFilteredBaseLightTarget(
                    a_leftProfile,
                    a_rightProfile,
                    a_settingPath.substr(std::string_view("filteredBaseLightAdjustments.").size()));
                break;
            case SettingFilterDomain::filteredObjectLighting:
                sharesTarget = false;
                break;
            case SettingFilterDomain::unfiltered:
                break;
            }
            return sharesTarget;
        }

        bool ProfilesShareSettingTargets(
            const std::string& a_leftProfile,
            const std::string& a_rightProfile,
            const std::string_view a_settingPath)
        {
            const auto domain = FilterDomainForSetting(a_settingPath);
            if (domain == SettingFilterDomain::filteredObjectLighting) return false;
            if (domain == SettingFilterDomain::weather &&
                SettingPathsOverlap("links.weather", a_settingPath))
            {
                const auto declaresSetting = [&](const std::string& a_profileName)
                {
                    const auto* profile = FindProfile(a_profileName);
                    return profile && std::ranges::any_of(
                                          profile->weatherMenuSettings,
                                          [&](const auto& a_declared)
                                          { return SettingPathsOverlap(a_declared, a_settingPath); });
                };
                if (!declaresSetting(a_leftProfile) || !declaresSetting(a_rightProfile))
                {
                    return false;
                }
            }
            if (domain == SettingFilterDomain::lighting)
            {
                const auto declaresSetting = [&](const std::string& a_profileName)
                {
                    const auto* profile = FindProfile(a_profileName);
                    return profile && std::ranges::any_of(
                                          profile->lightingMenuSettings,
                                          [&](const auto& a_declared)
                                          { return SettingPathsOverlap(a_declared, a_settingPath); });
                };
                if (!declaresSetting(a_leftProfile) || !declaresSetting(a_rightProfile))
                {
                    return false;
                }
            }
            if (domain == SettingFilterDomain::unfiltered ||
                !startupSettingTargetOverlapsCaptured)
            {
                return true;
            }
            const auto cached = startupSettingTargetOverlapCache.find(
                SettingTargetOverlapKey(a_leftProfile, a_rightProfile, a_settingPath));
            return cached != startupSettingTargetOverlapCache.end() ? cached->second : true;
        }

        void CaptureStartupSettingTargetOverlaps()
        {
            startupSettingTargetOverlapCache.clear();
            if (!RE::TESDataHandler::GetSingleton())
            {
                startupSettingTargetOverlapsCaptured = true;
                return;
            }

            std::vector<std::string> filteredWeatherPaths;
            std::vector<std::string> filteredLightingTemplatePaths;
            std::vector<std::string> filteredBaseLightPaths;
            for (const auto& profile : profiles)
            {
                for (const auto& rule : profile.filteredWeatherRules)
                {
                    AddSettingPath(filteredWeatherPaths, "filteredWeatherAdjustments." + rule.id);
                }
                for (const auto& rule : profile.filteredLightingTemplateRules)
                {
                    AddSettingPath(
                        filteredLightingTemplatePaths,
                        "filteredLightingTemplateAdjustments." + rule.id);
                }
                for (const auto& rule : profile.filteredBaseLightRules)
                {
                    AddSettingPath(filteredBaseLightPaths, "filteredBaseLightAdjustments." + rule.id);
                }
            }

            static constexpr std::array representativePaths{
                std::string_view{ "brightnessMultiplier" },
                std::string_view{ "lightBrightnessMultiplier" },
            };
            const auto cache = [&](const Profile& a_left, const Profile& a_right, const std::string_view a_path)
            {
                startupSettingTargetOverlapCache[SettingTargetOverlapKey(a_left.name, a_right.name, a_path)] =
                    ComputeProfilesShareSettingTargets(a_left.name, a_right.name, a_path);
            };
            for (std::size_t left = 0; left < profiles.size(); ++left)
            {
                for (std::size_t right = left + 1; right < profiles.size(); ++right)
                {
                    for (const auto path : representativePaths) cache(profiles[left], profiles[right], path);
                    for (const auto& path : filteredWeatherPaths) cache(profiles[left], profiles[right], path);
                    for (const auto& path : filteredLightingTemplatePaths) cache(profiles[left], profiles[right], path);
                    for (const auto& path : filteredBaseLightPaths) cache(profiles[left], profiles[right], path);
                }
            }
            startupSettingTargetOverlapsCaptured = true;
        }

        void RefreshSettingOwnershipCache()
        {
            if (settingOwnershipRevision == settingsRevision)
            {
                return;
            }

            settingOwnershipCache.clear();
            for (const auto& profile : profiles)
            {
                auto& layers = settingOwnershipCache[Lowercase(profile.name)].paths;
                std::string error;
                layers[0] = SettingPaths(ProfileDefaultsText(profile, error).value_or("{}"));
                layers[1] = SettingPaths(ActivePresetSettingsText(profile, error).value_or("{}"));
                layers[2] = SettingPaths(CurrentUserLayer(profile, error).value_or("{}"));
            }
            settingOwnershipRevision = settingsRevision;
        }

        void SortProfiles()
        {
            ++sliderBindingsRevision;
            std::ranges::sort(profiles, [](const Profile& a_left, const Profile& a_right)
            {
                if (a_left.priority != a_right.priority)
                {
                    return a_left.priority < a_right.priority;
                }
                const auto left = Lowercase(a_left.name);
                const auto right = Lowercase(a_right.name);
                return left != right ? left < right : a_left.name < a_right.name;
            });
        }

        void SynchronizeProfilePriorities()
        {
            for (auto& profile : profiles)
            {
                if (const auto cached = settingsCache.find(Lowercase(profile.name)); cached != settingsCache.end())
                {
                    profile.priority = cached->second.settings.profilePriority;
                }
            }
            SortProfiles();
        }
    }  // namespace

    void InvalidateDiscoveryCaches()
    {
        ++sliderBindingsRevision;
        WeatherPatcher::InvalidatePresetCache();
        discoveryInitialized = false;
        profiles.clear();
        pluginFilteredProfileDirectories.clear();
        settingsCache.clear();
        settingOwnershipCache.clear();
        startupSettingTargetOverlapCache.clear();
        startupSettingTargetOverlapsCaptured = false;
        settingOwnershipRevision = std::numeric_limits<std::uint64_t>::max();
        globalDefaultsCache.reset();
    }

    const std::vector<Profile>& GetProfiles()
    {
        if (runtimeStateReleased)
        {
            return profiles;
        }
        if (discoveryInitialized)
        {
            return profiles;
        }

        struct ProfileDisableRule
        {
            std::string source;
            std::vector<std::string> targets;
            bool enabled = false;
        };

        std::vector<ProfileDisableRule> disableRules;
        std::error_code error;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator iterator(kProfileRoot, options, error), end;
            iterator != end && !error;
            iterator.increment(error))
        {
            if (!iterator->is_directory(error))
            {
                continue;
            }
            const auto defaultPath = iterator->path() / kProfileDefaultsFile;
            std::string layoutError;
            const auto loadedDefaults = ProfileSettings::Load(iterator->path(), layoutError);
            if (!loadedDefaults)
            {
                logger::warn("[TuningUtil] profile settings rejected | path={} | {}", iterator->path().string(), layoutError);
                continue;
            }
            std::error_code layoutFileError;
            if (loadedDefaults->layers.empty() &&
                !std::filesystem::is_regular_file(iterator->path() / kMenuDefinitionFile, layoutFileError)) continue;
            const auto defaults = std::optional{ loadedDefaults->text };
            auto loadedLayout = ReadProfileLayout(iterator->path(), layoutError, true);
            if (!loadedLayout)
            {
                logger::warn("[TuningUtil] profile layout unavailable | path={} | {}", iterator->path().string(), layoutError);
                continue;
            }
            const auto name = ProfileName(loadedLayout->profile.empty() ?
                StringMember(*defaults, "profile").value_or("") : loadedLayout->profile);
            if (name.empty())
            {
                logger::warn(
                    "[TuningUtil] profile ignored | path={} | name invalid",
                    iterator->path().string());
                continue;
            }
            const auto combinedDefaults = JsonOverlay::Overlay(GlobalDefaultsText(), *defaults, layoutError);
            const auto parsedDefaults = combinedDefaults ? ParseSettings(*combinedDefaults, defaultPath) : std::nullopt;
            if (!parsedDefaults)
            {
                logger::warn("[TuningUtil] {} ignored | defaults invalid", name);
                continue;
            }
            if (std::ranges::any_of(profiles, [&](const Profile& a_profile)
                    { return Config::IEquals(a_profile.name, name); }))
            {
                logger::warn("[TuningUtil] {} ignored | duplicate | path={}", name, iterator->path().string());
                continue;
            }
            const auto document = Parse(*defaults);
            const auto dependencies = JsonStrings(
                document ? yyjson_doc_get_root(document.get()) : nullptr,
                "PluginDependency");
            if (pluginDependencyFilterReady && !PluginDependenciesSatisfied(dependencies))
            {
                pluginFilteredProfileDirectories.push_back(iterator->path());
                continue;
            }
            const auto disabledProfiles = JsonStrings(
                document ? yyjson_doc_get_root(document.get()) : nullptr,
                "DisableProfile");
            auto ambientAnchorWeather = Trim(
                StringMember(*defaults, "ambientAnchorWeather")
                    .value_or(std::string(kDefaultAmbientAnchorWeather)));
            if (ambientAnchorWeather.empty())
            {
                ambientAnchorWeather = kDefaultAmbientAnchorWeather;
            }
            auto runtimeCompressionAnchors = WeatherPatcher::ReadWeatherCompressionAnchors(ambientAnchorWeather);
            if (!runtimeCompressionAnchors && !Config::IEquals(ambientAnchorWeather, kDefaultAmbientAnchorWeather))
            {
                logger::warn(
                    "[TuningUtil] {} compression anchor weather unavailable | weather={} | fallback={}",
                    name,
                    ambientAnchorWeather,
                    kDefaultAmbientAnchorWeather);
                runtimeCompressionAnchors =
                    WeatherPatcher::ReadWeatherCompressionAnchors(kDefaultAmbientAnchorWeather);
            }
            const auto user = ReadText(kUserRoot / iterator->path().filename() / "userSettings.json");
            auto profileEnabled = parsedDefaults->EnableProfile;
            if (user)
            {
                profileEnabled = BooleanMember(*user, "EnableProfile").value_or(profileEnabled);
            }
            auto priority = IntegerMember(GlobalDefaultsText(), "profilePriority").value_or(0);
            priority = IntegerMember(*defaults, "profilePriority").value_or(priority);
            if (user)
            {
                priority = IntegerMember(*user, "profilePriority").value_or(priority);
            }
            auto layout = std::move(*loadedLayout);
            auto& lightingSliderLinks = layout.lightingLinks;
            profiles.push_back({
                .name = name,
                .priority = priority,
                .directory = iterator->path(),
                .ambientAnchorWeather = std::move(ambientAnchorWeather),
                .runtimeCompressionAnchors = runtimeCompressionAnchors,
                .disabledProfiles = disabledProfiles,
                .defaultSettingRoots = SettingRoots(*defaults),
                .sliderBindings = std::move(layout.bindings),
                .filteredWeatherRules = std::move(layout.weatherRules),
                .filteredLightingTemplateRules = std::move(layout.lightingRules),
                .filteredBaseLightRules = std::move(layout.baseLightRules),
                .filteredObjectLightingRules = std::move(layout.objectLightingRules),
                .lightingSliderSettings = std::move(lightingSliderLinks.settings),
                .customLightingSliderLinks = std::move(lightingSliderLinks.customLinks),
                .lightingMenuSettings = std::move(lightingSliderLinks.declaredSettings),
                .weatherMenuSettings = std::move(lightingSliderLinks.weatherDeclaredSettings),
                .hasMenuLayout = !layout.text.empty(),
            });
            disableRules.push_back({ name, std::move(disabledProfiles), profileEnabled });
        }

        if (pluginDependencyFilterReady)
        {
            std::unordered_map<std::string, std::string> disabledBy;
            for (const auto& rule : disableRules)
            {
                if (!rule.enabled) continue;
                for (const auto& target : rule.targets)
                {
                    if (Config::IEquals(rule.source, target)) continue;
                    const auto profile = std::ranges::find_if(profiles, [&](const Profile& a_profile)
                        { return Config::IEquals(a_profile.name, target); });
                    if (profile != profiles.end())
                    {
                        disabledBy.try_emplace(Lowercase(profile->name), rule.source);
                    }
                }
            }
            std::erase_if(profiles, [&](const Profile& a_profile)
            {
                const auto disabled = disabledBy.find(Lowercase(a_profile.name));
                if (disabled == disabledBy.end()) return false;
                pluginFilteredProfileDirectories.push_back(a_profile.directory);
                return true;
            });
        }

        SortProfiles();
        discoveryInitialized = true;
        return profiles;
    }

    bool IsProfilePluginFiltered(const std::filesystem::path& a_directory)
    {
        return std::ranges::contains(pluginFilteredProfileDirectories, a_directory);
    }

    std::vector<std::string> GetProfilesWithSettings(
        const std::span<const std::string_view> a_settingRoots)
    {
        std::vector<std::string> result;
        for (const auto& profile : GetProfiles())
        {
            if (OwnsAnySetting(profile, a_settingRoots))
            {
                result.push_back(profile.name);
            }
        }
        return result;
    }

    std::optional<std::string> GetExclusiveController(const ControlDomain a_domain)
    {
        std::optional<std::string> result;
        for (const auto& profile : GetProfiles())
        {
            auto profileName = profile.name;
            const auto& settings = GetSettings(profileName);
            if (settings.EnableProfile && ClaimsExclusiveControl(settings, a_domain))
            {
                result = profile.name;
            }
        }
        return result;
    }

    std::optional<std::string> GetOverridingProfile(
        const std::string& a_profileName,
        const std::span<const std::string> a_settingPaths)
    {
        const auto* currentProfile = FindProfile(a_profileName);
        auto currentName = a_profileName;
        if (!currentProfile || a_settingPaths.empty() || !GetSettings(currentName).EnableProfile)
        {
            return std::nullopt;
        }

        std::vector<std::string> requestedPaths;
        std::optional<std::string> exclusiveOverride;
        const auto addRequestedSetting = [&](const std::string_view a_setting, const bool a_includeNormalOverride)
        {
            if (exclusiveOverride) return;
            std::vector<std::string> expandedPaths;
            AddExpandedSettingPaths(expandedPaths, a_setting);
            for (const auto& expandedPath : expandedPaths)
            {
                const auto domains = ControlDomainsForSetting(expandedPath);
                auto allDomainsExclusivelyControlledByCurrent = !domains.empty();
                for (const auto domain : domains)
                {
                    const auto controller = GetExclusiveController(domain);
                    if (!controller)
                    {
                        allDomainsExclusivelyControlledByCurrent = false;
                        continue;
                    }
                    if (!Config::IEquals(*controller, currentProfile->name))
                    {
                        exclusiveOverride = std::move(controller);
                        return;
                    }
                }
                if (a_includeNormalOverride && !allDomainsExclusivelyControlledByCurrent)
                {
                    AddSettingPath(requestedPaths, expandedPath);
                }
            }
        };
        for (const auto& path : a_settingPaths)
        {
            if (IsProfileLocalSettingPath(path)) continue;
            if (path.starts_with("sliderValues."))
            {
                const auto valueID = std::string_view(path).substr(std::string_view("sliderValues.").size());
                for (const auto& binding : currentProfile->sliderBindings)
                {
                    if (!Config::IEquals(binding.valueID, valueID)) continue;
                    for (const auto& target : binding.targets)
                    {
                        addRequestedSetting(target.path, !SliderStorage::IsAdjustment(target.path));
                    }
                    if (!binding.ruleValueMap.empty())
                    {
                        addRequestedSetting(binding.ruleValueMap, false);
                    }
                }
            }
            else
            {
                addRequestedSetting(path, !SliderStorage::IsAdjustment(path));
            }
            if (exclusiveOverride) return exclusiveOverride;
        }
        if (requestedPaths.empty())
        {
            return std::nullopt;
        }

        RefreshSettingOwnershipCache();
        struct Owner
        {
            std::string profile;
            std::size_t rank = 0;
        };
        std::unordered_map<std::string, std::vector<Owner>> owners;
        std::size_t rank = 0;
        for (std::size_t layer = 0; layer < 3; ++layer)
        {
            for (const auto& profile : profiles)
            {
                auto profileName = profile.name;
                const auto cache = settingOwnershipCache.find(Lowercase(profile.name));
                if (!GetSettings(profileName).EnableProfile || cache == settingOwnershipCache.end())
                {
                    ++rank;
                    continue;
                }
                for (const auto& ownedPath : cache->second.paths[layer])
                {
                    if (std::ranges::any_of(requestedPaths, [&](const auto& requestedPath)
                            { return SettingPathsOverlap(ownedPath, requestedPath); }))
                    {
                        owners[ownedPath].push_back({ profile.name, rank });
                    }
                }
                ++rank;
            }
        }

        std::optional<Owner> overriding;
        for (const auto& [path, pathOwners] : owners)
        {
            std::optional<std::size_t> currentRank;
            for (const auto& owner : pathOwners)
            {
                if (Config::IEquals(owner.profile, currentProfile->name) &&
                    (!currentRank || owner.rank > *currentRank))
                {
                    currentRank = owner.rank;
                }
            }
            for (const auto& owner : pathOwners)
            {
                if (!Config::IEquals(owner.profile, currentProfile->name) &&
                    (!currentRank || owner.rank > *currentRank) &&
                    (!overriding || owner.rank > overriding->rank) &&
                    ProfilesShareSettingTargets(currentProfile->name, owner.profile, path))
                {
                    overriding = owner;
                }
            }
        }
        return overriding ? std::optional<std::string>{ overriding->profile } : std::nullopt;
    }

    int GetProfilePriority(const std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->priority : 0;
    }

    std::filesystem::path ProfileDirectory(const std::string& a_profileName)
    {
        if (const auto* profile = FindProfile(a_profileName))
        {
            return profile->directory;
        }
        return kProfileRoot / ProfileName(a_profileName);
    }

    const std::vector<FilteredWeatherRule>& GetFilteredWeatherRules(const std::string& a_profileName)
    {
        static const std::vector<FilteredWeatherRule> empty;
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->filteredWeatherRules : empty;
    }

    const FilteredWeatherRule* FindFilteredWeatherRule(
        const std::string& a_profileName,
        const std::string_view a_id)
    {
        const auto& rules = GetFilteredWeatherRules(a_profileName);
        const auto found = std::ranges::find_if(rules, [&](const FilteredWeatherRule& a_rule)
            { return Config::IEquals(a_rule.id, a_id); });
        return found != rules.end() ? std::addressof(*found) : nullptr;
    }

    const std::vector<FilteredLightingTemplateRule>& GetFilteredLightingTemplateRules(
        const std::string& a_profileName)
    {
        static const std::vector<FilteredLightingTemplateRule> empty;
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->filteredLightingTemplateRules : empty;
    }

    const FilteredLightingTemplateRule* FindFilteredLightingTemplateRule(
        const std::string& a_profileName,
        const std::string_view a_id)
    {
        const auto& rules = GetFilteredLightingTemplateRules(a_profileName);
        const auto found = std::ranges::find_if(rules, [&](const FilteredLightingTemplateRule& a_rule)
            { return Config::IEquals(a_rule.id, a_id); });
        return found != rules.end() ? std::addressof(*found) : nullptr;
    }

    const std::vector<FilteredBaseLightRule>& GetFilteredBaseLightRules(
        const std::string& a_profileName)
    {
        static const std::vector<FilteredBaseLightRule> empty;
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->filteredBaseLightRules : empty;
    }

    const FilteredBaseLightRule* FindFilteredBaseLightRule(
        const std::string& a_profileName,
        const std::string_view a_id)
    {
        const auto& rules = GetFilteredBaseLightRules(a_profileName);
        const auto found = std::ranges::find_if(rules, [&](const FilteredBaseLightRule& a_rule)
            { return Config::IEquals(a_rule.id, a_id); });
        return found != rules.end() ? std::addressof(*found) : nullptr;
    }

    const std::vector<FilteredObjectLightingRule>& GetFilteredObjectLightingRules(
        const std::string& a_profileName)
    {
        static const std::vector<FilteredObjectLightingRule> empty;
        const auto* profile = FindProfile(a_profileName);
        return profile ? profile->filteredObjectLightingRules : empty;
    }

    const FilteredObjectLightingRule* FindFilteredObjectLightingRule(
        const std::string& a_profileName,
        const std::string_view a_id)
    {
        const auto& rules = GetFilteredObjectLightingRules(a_profileName);
        const auto found = std::ranges::find_if(rules, [&](const FilteredObjectLightingRule& a_rule)
            { return Config::IEquals(a_rule.id, a_id); });
        return found != rules.end() ? std::addressof(*found) : nullptr;
    }

    LightingPatcher::LightingLinks ResolveLightingSliderLinks(
        const std::span<const std::string> a_profileNames,
        const std::string_view a_settingRoot,
        const LightingPatcher::LightingLinks& a_fallback)
    {
        auto result = a_fallback;
        const auto prefix = Lowercase(std::string(a_settingRoot)) + '.';
        for (const auto& profileName : a_profileNames)
        {
            const auto* profile = FindProfile(profileName);
            if (!profile) continue;
            for (const auto& setting : profile->lightingSliderSettings)
            {
                const auto key = Lowercase(setting);
                if (!key.starts_with(prefix)) continue;
                if (const auto custom = profile->customLightingSliderLinks.find(key);
                    custom != profile->customLightingSliderLinks.end())
                    result = custom->second;
            }
        }
        return result;
    }

    std::map<std::string, LightingPatcher::LightingLinks, std::less<>> ResolveLightingSliderLinkOverrides(
        const std::span<const std::string> a_profileNames,
        const std::string_view a_settingRoot)
    {
        std::map<std::string, LightingPatcher::LightingLinks, std::less<>> result;
        const auto prefix = Lowercase(std::string(a_settingRoot)) + '.';
        for (const auto& profileName : a_profileNames)
        {
            const auto* profile = FindProfile(profileName);
            if (!profile) continue;
            for (const auto& setting : profile->lightingSliderSettings)
            {
                const auto key = Lowercase(setting);
                if (!key.starts_with(prefix)) continue;
                const auto target = key.substr(prefix.size());
                if (const auto custom = profile->customLightingSliderLinks.find(key);
                    custom != profile->customLightingSliderLinks.end())
                    result.insert_or_assign(target, custom->second);
                else
                    result.erase(target);
            }
        }
        return result;
    }

    bool ReloadFilteredRules(const bool a_force, const ProfileLayoutValidator& a_validate)
    {
        auto changed = false;
        for (auto& profile : profiles)
        {
            std::string layoutError;
            auto layout = ReadProfileLayout(profile.directory, layoutError, !profile.hasMenuLayout);
            if (!layout)
            {
                logger::warn("[TuningUtil] profile layout reload rejected | profile={} | {}", profile.name, layoutError);
                continue;
            }
            if (!layout->profile.empty() && !Config::IEquals(ProfileName(layout->profile), profile.name))
            {
                logger::warn("[TuningUtil] profile identity changed | profile={} | restart required", profile.name);
                continue;
            }
            if (a_validate && !layout->text.empty() && !a_validate(profile, layout->text)) continue;
            auto& weatherRules = layout->weatherRules;
            auto& sliderBindings = layout->bindings;
            auto& lightingRules = layout->lightingRules;
            auto& baseLightRules = layout->baseLightRules;
            auto& objectLightingRules = layout->objectLightingRules;
            auto& lightingSliderLinks = layout->lightingLinks;
            if (!a_force && profile.hasMenuLayout == !layout->text.empty() &&
                sliderBindings == profile.sliderBindings && weatherRules == profile.filteredWeatherRules &&
                lightingRules == profile.filteredLightingTemplateRules &&
                baseLightRules == profile.filteredBaseLightRules &&
                objectLightingRules == profile.filteredObjectLightingRules &&
                lightingSliderLinks.settings == profile.lightingSliderSettings &&
                lightingSliderLinks.customLinks == profile.customLightingSliderLinks &&
                lightingSliderLinks.declaredSettings == profile.lightingMenuSettings &&
                lightingSliderLinks.weatherDeclaredSettings == profile.weatherMenuSettings)
            {
                continue;
            }
            std::optional<Settings> runtimeSettings;
            std::optional<std::string> remappedSettings;
            std::optional<std::string> presetPreviewUserLayer;
            if (const auto cached = settingsCache.find(Lowercase(profile.name)); cached != settingsCache.end())
            {
                runtimeSettings = cached->second.settings;
                presetPreviewUserLayer = cached->second.presetPreviewUserLayer;
                std::string error;
                remappedSettings = SliderStorage::Remap(SerializeSettings(*runtimeSettings), profile.sliderBindings, sliderBindings, error);
                if (presetPreviewUserLayer)
                    presetPreviewUserLayer = SliderStorage::Remap(*presetPreviewUserLayer, profile.sliderBindings, sliderBindings, error);
            }
            WeatherPatcher::RemapPresetSliderValues(profile.name, profile.sliderBindings, sliderBindings);
            ++sliderBindingsRevision;
            profile.hasMenuLayout = !layout->text.empty();
            profile.sliderBindings = std::move(sliderBindings);
            profile.filteredWeatherRules = std::move(weatherRules);
            profile.filteredLightingTemplateRules = std::move(lightingRules);
            profile.filteredBaseLightRules = std::move(baseLightRules);
            profile.filteredObjectLightingRules = std::move(objectLightingRules);
            profile.lightingSliderSettings = std::move(lightingSliderLinks.settings);
            profile.customLightingSliderLinks = std::move(lightingSliderLinks.customLinks);
            profile.lightingMenuSettings = std::move(lightingSliderLinks.declaredSettings);
            profile.weatherMenuSettings = std::move(lightingSliderLinks.weatherDeclaredSettings);
            settingsCache.erase(Lowercase(profile.name));
            auto name = profile.name;
            auto& reloaded = GetSettings(name);
            if (runtimeSettings)
            {
                if (remappedSettings)
                {
                    std::string error;
                    const auto merged = JsonOverlay::Overlay(SerializeProfileSettings(profile, reloaded), *remappedSettings, error);
                    if (auto parsed = merged ? ParseProfileSettings(*merged, profile) : std::nullopt)
                        runtimeSettings = std::move(*parsed);
                }
                reloaded = std::move(*runtimeSettings);
                settingsCache.at(Lowercase(profile.name)).presetPreviewUserLayer = std::move(presetPreviewUserLayer);
                ++settingsRevision;
            }
            changed = true;
        }
        return changed;
    }

    bool SetSliderCreatorPreview(
        std::string& a_profileName,
        const std::string_view a_existingRuleID,
        const SliderCreator::Definition& a_definition,
        const double a_value,
        bool& a_changed,
        std::string& a_error,
        SettingsUpdate::Targets& a_targets)
    {
        constexpr std::string_view previewRuleID = "$sliderCreatorPreview";
        a_changed = false;
        a_targets = SettingsUpdate::Targets::none;
        a_error.clear();
        if (a_definition.settings.empty() || !std::isfinite(a_value) ||
            std::ranges::any_of(a_definition.settings, [](const auto& a_target)
                { return !SliderSettingCatalog::Find(a_target.setting); }))
        {
            a_error = "The functional preview requires a valid slider.";
            return false;
        }

        const auto profileName = ProfileName(a_profileName);
        (void)GetProfiles();
        const auto profile = std::ranges::find_if(profiles, [&](const Profile& a_profile)
            { return Config::IEquals(a_profile.name, profileName); });
        if (profile == profiles.end())
        {
            a_error = "The slider profile is unavailable.";
            return false;
        }

        auto settingsName = profile->name;
        auto& settings = GetSettings(settingsName);
        const auto runtimeID = a_existingRuleID.empty() ? std::string(previewRuleID) : std::string(a_existingRuleID);
        auto binding = std::ranges::find(profile->sliderBindings, runtimeID, &SliderStorage::Binding::controlID);
        if (binding != profile->sliderBindings.end()) a_targets = SettingsUpdate::ForBinding(*binding);
        if (binding == profile->sliderBindings.end())
        {
            SliderStorage::Binding preview;
            preview.id = runtimeID;
            preview.valueID = runtimeID;
            preview.controlID = runtimeID;
            preview.ruleID = runtimeID;
            profile->sliderBindings.push_back(std::move(preview));
            ++sliderBindingsRevision;
            binding = std::prev(profile->sliderBindings.end());
            a_changed = true;
        }
        std::vector<SliderStorage::Target> targets;
        for (const auto& target : a_definition.settings) targets.push_back({ target.setting, target.scale });
        std::string map;
        if (a_definition.filtered)
        {
            switch (a_definition.filterDomain)
            {
            case SliderCreator::FilterDomain::weather: map = "filteredWeatherAdjustments"; break;
            case SliderCreator::FilterDomain::lightingTemplate: map = "filteredLightingTemplateAdjustments"; break;
            case SliderCreator::FilterDomain::baseLight: map = "filteredBaseLightAdjustments"; break;
            case SliderCreator::FilterDomain::baseObject: map = "filteredObjectLightingAdjustments"; break;
            }
        }
        if (binding->targets != targets || binding->ruleValueMap != map) ++sliderBindingsRevision;
        a_changed |= binding->targets != targets || binding->ruleValueMap != map ||
            !settings.sliderValues.contains(binding->valueID) || settings.sliderValues[binding->valueID] != a_value;
        binding->targets = std::move(targets);
        binding->ruleValueMap = std::move(map);
        a_targets = a_targets | SettingsUpdate::ForBinding(*binding);
        binding->neutral = SliderStorage::Neutral(binding->targets.front().path);
        settings.sliderValues[binding->valueID] = a_value;
        const auto filter = [](const SliderCreator::Filter& a_filter)
        {
            return WeatherFilter{
                .formIDs = a_filter.formIDs,
                .contains = a_filter.contains,
            };
        };
        const auto eraseRule = [&](auto& a_rules)
        {
            const auto originalSize = a_rules.size();
            std::erase_if(a_rules, [&](const auto& a_rule)
                { return Config::IEquals(a_rule.id, runtimeID); });
            if (a_rules.size() != originalSize) ++sliderBindingsRevision;
            return a_rules.size() != originalSize;
        };
        const auto findValue = [&](auto& a_values)
        {
            return std::ranges::find_if(a_values, [&](const auto& a_entry)
                { return Config::IEquals(a_entry.first, runtimeID); });
        };
        const auto assignValue = [&](auto& a_values)
        {
            const auto existing = findValue(a_values);
            if (existing != a_values.end()) existing->second = a_value;
            else a_values.emplace(runtimeID, a_value);
        };
        const auto otherRuleExists = [&](const auto& a_rules)
        {
            return std::ranges::any_of(a_rules, [&](const auto& a_rule)
                { return Config::IEquals(a_rule.id, runtimeID); });
        };

        if (!a_definition.filtered)
        {
            a_changed |= eraseRule(profile->filteredWeatherRules);
            a_changed |= eraseRule(profile->filteredLightingTemplateRules);
            a_changed |= eraseRule(profile->filteredBaseLightRules);
            a_changed |= eraseRule(profile->filteredObjectLightingRules);
            return true;
        }
        if (a_definition.filterDomain == SliderCreator::FilterDomain::weather)
        {
            FilteredWeatherRule rule{
                .id = runtimeID,
                .controlID = runtimeID,
                .customLinks = CreatorWeatherLinks(a_definition.customLinks),
                .ignoreProfileFilters = a_definition.ignoreProfileFilters,
                .hueScales = CreatorHueScales(a_definition.hueScales),
            };
            std::optional<FilteredWeatherOperation> operation;
            for (const auto& target : a_definition.settings)
            {
                auto parsed = ParseFilteredWeatherSetting(target.setting);
                if (!parsed || (operation && *operation != parsed->setting.operation))
                {
                    a_error = "The functional preview settings do not use one compatible weather operation.";
                    return false;
                }
                operation = parsed->setting.operation;
                parsed->setting.scale = target.scale;
                rule.settings.push_back(std::move(parsed->setting));
            }
            rule.times = a_definition.times;
            if (!a_definition.useTimes) rule.times.fill(true);
            if (!std::ranges::any_of(rule.times, std::identity{}))
            {
                a_error = "Select at least one time for the functional preview.";
                return false;
            }
            rule.include = filter(a_definition.include);
            rule.exclude = filter(a_definition.exclude);
            rule.defaultValue = *operation == FilteredWeatherOperation::hueShift ? 0.0 : 1.0;

            const auto existing = std::ranges::find_if(profile->filteredWeatherRules, [&](const auto& a_rule)
                { return Config::IEquals(a_rule.id, runtimeID); });
            const auto value = findValue(settings.filteredWeatherAdjustments);
            if (existing != profile->filteredWeatherRules.end() && *existing == rule &&
                value != settings.filteredWeatherAdjustments.end() &&
                std::abs(value->second - a_value) <= 0.000001 &&
                !otherRuleExists(profile->filteredLightingTemplateRules) &&
                !otherRuleExists(profile->filteredBaseLightRules) &&
                !otherRuleExists(profile->filteredObjectLightingRules))
                return true;

            eraseRule(profile->filteredWeatherRules);
            eraseRule(profile->filteredLightingTemplateRules);
            eraseRule(profile->filteredBaseLightRules);
            eraseRule(profile->filteredObjectLightingRules);
            profile->filteredWeatherRules.push_back(std::move(rule));
            ++sliderBindingsRevision;
            assignValue(settings.filteredWeatherAdjustments);
        }
        else if (a_definition.filterDomain == SliderCreator::FilterDomain::lightingTemplate)
        {
            FilteredLightingTemplateRule rule{
                .id = runtimeID,
                .controlID = runtimeID,
                .customLinks = CreatorLightingLinks(a_definition.customLinks),
                .ignoreProfileFilters = a_definition.ignoreProfileFilters,
                .hueScales = CreatorHueScales(a_definition.hueScales),
            };
            std::optional<FilteredLightingTemplateOperation> operation;
            for (const auto& target : a_definition.settings)
            {
                auto setting = ParseFilteredLightingTemplateSetting(target.setting);
                if (!setting || (operation && *operation != setting->operation))
                {
                    a_error = "The functional preview settings do not use one compatible Lighting Template operation.";
                    return false;
                }
                operation = setting->operation;
                setting->scale = target.scale;
                rule.settings.push_back(std::move(*setting));
            }
            rule.include = filter(a_definition.include);
            rule.exclude = filter(a_definition.exclude);
            rule.locationTypeInclusions = a_definition.include.locationTypes;
            rule.locationTypeExclusions = a_definition.exclude.locationTypes;
            rule.inclusionMultiLocationExceptions = a_definition.include.multiLocationExceptions;
            rule.exclusionMultiLocationExceptions = a_definition.exclude.multiLocationExceptions;
            rule.defaultValue = 1.0;

            const auto existing = std::ranges::find_if(profile->filteredLightingTemplateRules, [&](const auto& a_rule)
                { return Config::IEquals(a_rule.id, runtimeID); });
            const auto value = findValue(settings.filteredLightingTemplateAdjustments);
            if (existing != profile->filteredLightingTemplateRules.end() && *existing == rule &&
                value != settings.filteredLightingTemplateAdjustments.end() &&
                std::abs(value->second - a_value) <= 0.000001 &&
                !otherRuleExists(profile->filteredWeatherRules) &&
                !otherRuleExists(profile->filteredBaseLightRules) &&
                !otherRuleExists(profile->filteredObjectLightingRules))
                return true;

            eraseRule(profile->filteredWeatherRules);
            eraseRule(profile->filteredLightingTemplateRules);
            eraseRule(profile->filteredBaseLightRules);
            eraseRule(profile->filteredObjectLightingRules);
            profile->filteredLightingTemplateRules.push_back(std::move(rule));
            ++sliderBindingsRevision;
            assignValue(settings.filteredLightingTemplateAdjustments);
        }
        else if (a_definition.filterDomain == SliderCreator::FilterDomain::baseLight)
        {
            FilteredBaseLightRule rule{
                .id = runtimeID,
                .controlID = runtimeID,
                .priority = a_definition.priority,
                .hueFilter = a_definition.hueFilter,
                .useXemiFilter = a_definition.useXemiFilter ||
                    !a_definition.xemiInclude.formIDs.empty() || !a_definition.xemiInclude.contains.empty() ||
                    !a_definition.xemiExclude.formIDs.empty() || !a_definition.xemiExclude.contains.empty(),
                .xemiInclude = filter(a_definition.xemiInclude),
                .xemiExclude = filter(a_definition.xemiExclude),
                .hueScales = CreatorHueScales(a_definition.hueScales),
            };
            if (!HueFilter::Valid(rule.hueFilter))
            {
                a_error = "Select valid hue bands for the Hue Filter.";
                return false;
            }
            std::optional<FilteredBaseLightOperation> operation;
            for (const auto& target : a_definition.settings)
            {
                auto setting = ParseFilteredBaseLightSetting(target.setting);
                if (!setting || (operation && *operation != setting->operation))
                {
                    a_error = "The functional preview settings do not use one compatible Base Light operation.";
                    return false;
                }
                operation = setting->operation;
                setting->scale = target.scale;
                rule.settings.push_back(std::move(*setting));
            }
            rule.include = filter(a_definition.include);
            rule.exclude = filter(a_definition.exclude);
            if (rule.useXemiFilter && *operation != FilteredBaseLightOperation::brightness &&
                *operation != FilteredBaseLightOperation::radius)
            {
                a_error = "XEMI filters apply only to Object Effect Lighting and Point Light Brightness or Radius sliders with record filters.";
                return false;
            }
            rule.defaultValue = *operation == FilteredBaseLightOperation::hueShift ? 0.0 : 1.0;

            const auto existing = std::ranges::find_if(profile->filteredBaseLightRules, [&](const auto& a_rule)
                { return Config::IEquals(a_rule.id, runtimeID); });
            const auto value = findValue(settings.filteredBaseLightAdjustments);
            if (existing != profile->filteredBaseLightRules.end() && *existing == rule &&
                value != settings.filteredBaseLightAdjustments.end() &&
                std::abs(value->second - a_value) <= 0.000001 &&
                !otherRuleExists(profile->filteredWeatherRules) &&
                !otherRuleExists(profile->filteredLightingTemplateRules) &&
                !otherRuleExists(profile->filteredObjectLightingRules))
                return true;

            eraseRule(profile->filteredWeatherRules);
            eraseRule(profile->filteredLightingTemplateRules);
            eraseRule(profile->filteredBaseLightRules);
            eraseRule(profile->filteredObjectLightingRules);
            profile->filteredBaseLightRules.push_back(std::move(rule));
            ++sliderBindingsRevision;
            assignValue(settings.filteredBaseLightAdjustments);
        }
        else
        {
            FilteredObjectLightingRule rule{
                .id = runtimeID,
                .controlID = runtimeID,
                .priority = a_definition.priority,
            };
            for (const auto& target : a_definition.settings)
            {
                auto setting = ParseFilteredObjectLightingSetting(target.setting);
                if (!setting)
                {
                    a_error = "The functional preview settings do not use an Object Effect Lighting setting.";
                    return false;
                }
                setting->scale = target.scale;
                rule.settings.push_back(std::move(*setting));
            }
            rule.include = filter(a_definition.include);
            rule.exclude = filter(a_definition.exclude);

            rule.xemiInclude = filter(a_definition.xemiInclude);
            rule.xemiExclude = filter(a_definition.xemiExclude);
            rule.interiorOnly = a_definition.interiorOnly;
            rule.notInterior = a_definition.notInterior;
            rule.cellInclude = filter(a_definition.cellInclude);
            rule.cellExclude = filter(a_definition.cellExclude);
            const auto existing = std::ranges::find_if(profile->filteredObjectLightingRules, [&](const auto& a_rule)
                { return Config::IEquals(a_rule.id, runtimeID); });
            const auto value = findValue(settings.filteredObjectLightingAdjustments);
            if (existing != profile->filteredObjectLightingRules.end() && *existing == rule &&
                value != settings.filteredObjectLightingAdjustments.end() &&
                std::abs(value->second - a_value) <= 0.000001 &&
                !otherRuleExists(profile->filteredWeatherRules) &&
                !otherRuleExists(profile->filteredLightingTemplateRules) &&
                !otherRuleExists(profile->filteredBaseLightRules))
                return true;

            eraseRule(profile->filteredWeatherRules);
            eraseRule(profile->filteredLightingTemplateRules);
            eraseRule(profile->filteredBaseLightRules);
            eraseRule(profile->filteredObjectLightingRules);
            profile->filteredObjectLightingRules.push_back(std::move(rule));
            ++sliderBindingsRevision;
            assignValue(settings.filteredObjectLightingAdjustments);
        }

        a_changed = true;
        ++settingsRevision;
        return true;
    }

    Settings& GetSettings(std::string& a_profileName)
    {
        static Settings rejected{};
        const auto name = ProfileName(a_profileName);
        const auto* profile = FindProfile(name);
        if (name.empty() || !profile)
        {
            logger::warn("[TuningUtil] settings access rejected | profile={} unknown", a_profileName);
            return rejected;
        }
        const auto cacheKey = Lowercase(name);
        if (const auto cached = settingsCache.find(cacheKey); cached != settingsCache.end())
        {
            return cached->second.settings;
        }

        auto settings = LoadStoredSettings(*profile);
        if (!settings)
        {
            logger::warn("[TuningUtil] {} defaults unreadable | fallback=neutral", name);
            return settingsCache.try_emplace(cacheKey, CachedSettings{}).first->second.settings;
        }
        return settingsCache.try_emplace(cacheKey, std::move(*settings)).first->second.settings;
    }

    void InvalidatePreparedProfileStack(const std::string_view a_profile)
    {
        if (a_profile.empty())
        {
            for (auto& [name, cached] : settingsCache) cached.preparedStack.reset();
        }
        else if (const auto cached = settingsCache.find(Lowercase(std::string(a_profile))); cached != settingsCache.end())
            cached->second.preparedStack.reset();
    }

    Settings ResolveSettingsStack(const std::span<const std::string> a_profileNames)
    {
        std::string error;
        auto combined = GlobalDefaultsText();
        std::vector<std::pair<std::string_view, const PreparedProfileStack*>> prepared;
        for (const auto& profileName : a_profileNames)
        {
            const auto* profile = FindProfile(profileName);
            if (profile) prepared.emplace_back(profile->name, std::addressof(PreparedStack(*profile)));
        }
        for (const auto& [profileName, contribution] : prepared)
        {
            const auto& defaults = contribution->defaults;
            if (!defaults)
            {
                continue;
            }
            if (auto overlaid = JsonOverlay::Overlay(combined, *defaults, error))
            {
                combined = std::move(*overlaid);
            }
            else
            {
                logger::warn("[TuningUtil] {} stack defaults failed | {}", profileName, error);
            }
        }
        for (const auto& [profileName, contribution] : prepared)
        {
            const auto& activePresets = contribution->presets;
            if (!activePresets)
            {
                continue;
            }
            if (auto overlaid = JsonOverlay::Overlay(combined, *activePresets, error))
            {
                combined = std::move(*overlaid);
            }
            else
            {
                logger::warn("[TuningUtil] {} stack presets failed | {}", profileName, error);
            }
        }
        for (const auto& [profileName, contribution] : prepared)
        {
            const auto& user = contribution->user;
            if (!user)
            {
                continue;
            }
            if (auto overlaid = JsonOverlay::Overlay(combined, *user, error))
            {
                combined = std::move(*overlaid);
            }
            else
            {
                logger::warn("[TuningUtil] {} stack user settings failed | {}", profileName, error);
            }
        }
        std::vector<std::string> contributions;
        for (const auto& [profileName, contribution] : prepared) contributions.push_back(contribution->adjustments);
        const auto stacked = SliderStorage::Stack(contributions, error);
        const auto& paths = SliderStorage::AdjustmentPaths();
        const auto adjustments = stacked ? JsonOverlay::ProjectPaths(*stacked, paths, error) : std::nullopt;
        if (adjustments)
            if (auto resolved = JsonOverlay::Overlay(combined, *adjustments, error)) combined = std::move(*resolved);
        return ParseSettings(combined, "resolved profile stack").value_or(Settings{});
    }

    WeatherPatcher::WeatherCompressionAnchors ResolveCompressionAnchors(
        const std::span<const std::string> a_profileNames)
    {
        const Profile* anchorProfile = nullptr;
        for (const auto& profileName : a_profileNames)
        {
            if (const auto* profile = FindProfile(profileName)) anchorProfile = profile;
        }
        return anchorProfile && anchorProfile->runtimeCompressionAnchors ?
                   *anchorProfile->runtimeCompressionAnchors :
                   WeatherPatcher::WeatherCompressionAnchors{};
    }

    std::optional<std::string> SerializePresetSettings(
        std::string& a_profileName,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto defaultText = profile ? LocalDefaultsText(*profile, a_error) : std::nullopt;
        if (!profile || !defaultText || !ParseSettings(*defaultText, ProfileDefaultsPath(*profile)))
        {
            a_error = "The profile defaults could not be read.";
            return std::nullopt;
        }

        const auto current = SerializeProfileSettings(*profile, GetSettings(a_profileName));
        const auto declared = CombineDeclaredSettings(current, *defaultText, a_error);
        if (!declared)
        {
            a_error = "The profile settings could not be prepared for a preset.";
            return std::nullopt;
        }

        const auto difference = JsonOverlay::Difference(*declared, *defaultText, a_error);
        if (!difference)
        {
            a_error = "The profile settings could not be compared with the defaults.";
            return std::nullopt;
        }

        static const std::vector<std::string> presetExcludedSettings{
            "profilePriority",
            "EnableProfile",
            "ShowAdvanced",
        };
        const auto preset = JsonOverlay::RemovePaths(*difference, presetExcludedSettings, a_error);
        if (!preset)
        {
            a_error = "Profile-only settings could not be removed from the preset.";
            return std::nullopt;
        }
        return JsonFormatting::Pretty(*preset);
    }

    std::optional<std::string> ResolvePresetResetSettings(
        std::string& a_profileName,
        const std::string_view a_effectivePresetSettings,
        const std::string_view a_resetSchema,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto defaults = profile ? LocalDefaultsText(*profile, a_error) : std::nullopt;
        const auto storedEffective = profile ? SliderStorage::Store(a_effectivePresetSettings, profile->sliderBindings, false, a_error) : std::nullopt;
        const auto storedSchema = profile ? SliderStorage::Store(a_resetSchema, profile->sliderBindings, false, a_error) : std::nullopt;
        const auto effective = defaults ?
                                   (storedEffective ? JsonOverlay::ProjectLike(*storedEffective, *defaults, a_error) : std::nullopt) :
                                   std::nullopt;
        const auto fallback = effective ?
                                  JsonOverlay::Overlay(*defaults, *effective, a_error) :
                                  std::nullopt;
        const auto reset = fallback && storedSchema ?
                               JsonOverlay::ProjectLike(*fallback, *storedSchema, a_error) :
                               std::nullopt;
        if (!profile || !reset)
        {
            if (a_error.empty()) a_error = "The preset defaults could not be resolved.";
            return std::nullopt;
        }
        return reset;
    }

    PresetSelections GetDefaultPresetSelections(
        std::string& a_profileName,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto defaults = profile ? ProfileDefaultsText(*profile, a_error) : std::nullopt;
        if (!defaults)
        {
            if (a_error.empty()) a_error = "The profile defaults could not be read.";
            return {};
        }
        const auto selections = UserSettings::ResolvePresetSelections(*defaults, std::nullopt, a_error);
        return selections.value_or(PresetSelections{});
    }

    PresetSelections GetSavedPresetSelections(
        std::string& a_profileName,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        if (!profile)
        {
            a_error = "The profile is unavailable.";
            return {};
        }
        const auto defaults = ProfileDefaultsText(*profile, a_error);
        const auto user = ReadText(UserSettingsPath(*profile));
        if (!defaults)
        {
            a_error = "The profile defaults could not be read.";
            return {};
        }
        const auto selections = UserSettings::ResolvePresetSelections(
            *defaults, user ? std::optional<std::string_view>{ *user } : std::nullopt, a_error);
        return selections.value_or(PresetSelections{});
    }

    bool SavePresetSelectionSnapshot(
        std::string& a_profileName,
        const PresetSelections& a_selections,
        const std::string_view a_changedSettings,
        std::string& a_error,
        const std::string_view a_trigger,
        const std::source_location a_source)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto defaults = profile ? LocalDefaultsText(*profile, a_error) : std::nullopt;
        const auto storedChanges = profile ? SliderStorage::Store(a_changedSettings, profile->sliderBindings, false, a_error) : std::nullopt;
        const auto changed = defaults && storedChanges ?
                                 JsonOverlay::ProjectLike(*storedChanges, *defaults, a_error) :
                                 std::nullopt;
        const auto existingText = profile ? ReadText(UserSettingsPath(*profile)).value_or("{}") : "{}";
        const auto existing = changed ? UserSettingsValuesOnly(existingText, a_error) : std::nullopt;
        const auto updated = existing ? JsonOverlay::Overlay(*existing, *changed, a_error) : std::nullopt;
        const auto effective = updated ? JsonOverlay::Overlay(*defaults, *updated, a_error) : std::nullopt;
        if (!profile || !effective || !ParseSettings(*effective, UserSettingsPath(*profile)))
        {
            if (a_error.empty()) a_error = "The preset selection snapshot could not be prepared.";
            return false;
        }
        if (!WriteUserSettings(*profile, *updated, a_selections))
        {
            if (a_error.empty()) a_error = "The preset selection snapshot could not be saved.";
            return false;
        }

        WeatherPatcher::InvalidatePresetCache();
        settingsCache.erase(Lowercase(profile->name));
        (void)GetSettings(a_profileName);
        ApplySettings(std::format("{} | profile={}", a_trigger, a_profileName), true, a_source);
        return true;
    }

    bool ApplyPresetPreview(
        std::string& a_profileName,
        const std::string_view a_effectivePresetSettings,
        const std::string_view a_changedPresetSettings,
        std::string& a_error,
        const std::string_view a_trigger,
        const std::source_location a_source)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        if (!profile)
        {
            a_error = "The profile is unavailable.";
            return false;
        }

        (void)GetSettings(a_profileName);
        const auto cached = settingsCache.find(Lowercase(profile->name));
        if (cached == settingsCache.end())
        {
            a_error = "The profile settings cache is unavailable.";
            return false;
        }

        auto userLayer = cached->second.presetPreviewUserLayer;
        if (!userLayer)
        {
            userLayer = CurrentUserLayer(*profile, a_error);
        }
        const auto storedEffective = SliderStorage::Store(a_effectivePresetSettings, profile->sliderBindings, false, a_error);
        const auto storedChanged = SliderStorage::Store(a_changedPresetSettings, profile->sliderBindings, false, a_error);
        const auto effective = storedEffective ? JsonOverlay::ProjectLike(*storedEffective, cached->second.localDefaults, a_error) : std::nullopt;
        const auto changed = effective && storedChanged ? JsonOverlay::ProjectLike(*storedChanged, cached->second.localDefaults, a_error) : std::nullopt;
        const auto presetDefaults = changed ? JsonOverlay::Overlay(cached->second.localDefaults, *effective, a_error) : std::nullopt;
        const auto withUser = presetDefaults && userLayer ? JsonOverlay::Overlay(*presetDefaults, *userLayer, a_error) : std::nullopt;
        const auto preview = withUser ? JsonOverlay::Overlay(*withUser, *changed, a_error) : std::nullopt;
        auto settings = preview ? ParseProfileSettings(*preview, *profile) : std::nullopt;
        if (!settings)
        {
            if (a_error.empty()) a_error = "The preset preview settings could not be composed.";
            return false;
        }

        cached->second.settings = std::move(*settings);
        cached->second.presetPreviewUserLayer = std::move(userLayer);
        ApplySettings(std::format("{} | profile={}", a_trigger, a_profileName), true, a_source);
        return true;
    }

    bool SaveSettings(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        if (!profile || !WriteSettings(*profile, a_profileName, GetSettings(a_profileName))) return false;
        WeatherPatcher::InvalidatePresetSelections(a_profileName);
        return true;
    }

    bool SaveProfileSetupSettings(
        std::string& a_profileName,
        const ProfileSetup::Domain a_domain,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto& paths = ProfileSetup::SettingPaths(a_domain);
        const auto setup = profile ?
                               JsonOverlay::ProjectPaths(
                                   SerializeSettings(GetSettings(a_profileName)),
                                   paths,
                                   a_error) :
                               std::nullopt;
        const auto defaults = setup ?
                                  JsonOverlay::ProjectPaths(GlobalDefaultsText(), paths, a_error) :
                                  std::nullopt;
        const auto differences = defaults ?
                                     JsonOverlay::Difference(*setup, *defaults, a_error) :
                                     std::nullopt;
        if (!profile || !differences ||
            !WriteProfileSetupPatch(*profile, *differences, a_error, paths))
        {
            if (a_error.empty()) a_error = "The profile setup could not be saved.";
            return false;
        }

        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            const auto localDefaults = LocalDefaultsText(*profile, a_error);
            const auto presetDefaults = localDefaults ?
                                            PresetDefaultsText(*profile, a_error) :
                                            std::nullopt;
            if (!localDefaults || !presetDefaults) return false;
            cached->second.localDefaults = *localDefaults;
            cached->second.presetDefaults = *presetDefaults;
            cached->second.preparedStack.reset();
        }
        a_profileName = profile->name;
        return true;
    }

    bool RestoreProfileSetupSettings(
        std::string& a_profileName,
        const ProfileSetup::Domain a_domain,
        std::string& a_error)
    {
        a_error.clear();
        const auto* profile = FindProfile(a_profileName);
        const auto defaults = profile ? LocalDefaultsText(*profile, a_error) : std::nullopt;
        const auto setup = defaults ?
                               JsonOverlay::ProjectPaths(
                                   *defaults,
                                   ProfileSetup::SettingPaths(a_domain),
                                   a_error) :
                               std::nullopt;
        const auto current = profile ? SerializeSettings(GetSettings(a_profileName)) : std::string{};
        const auto restored = setup ? JsonOverlay::Overlay(current, *setup, a_error) : std::nullopt;
        auto settings = restored ?
                            ParseSettings(*restored, ProfileDefaultsPath(*profile)) :
                            std::nullopt;
        if (!profile || !settings)
        {
            if (a_error.empty()) a_error = "The saved profile setup could not be restored.";
            return false;
        }

        GetSettings(a_profileName) = std::move(*settings);
        ApplySettings(std::format("setup-restore | profile={} | domain={}", a_profileName,
            a_domain == ProfileSetup::Domain::weather ? "weather" : "lighting"));
        a_profileName = profile->name;
        return true;
    }

    bool SaveSettingsAsProfileDefaults(
        std::string& a_profileName,
        const std::vector<std::string>& a_scopes,
        std::string& a_error)
    {
        a_error.clear();
        (void)GetProfiles();
        const auto profile = std::ranges::find_if(profiles, [&](const Profile& a_candidate)
            { return Config::IEquals(a_candidate.name, a_profileName); });
        if (profile == profiles.end())
        {
            a_error = "The profile is unavailable.";
            return false;
        }

        const auto canonicalName = profile->name;
        auto resolvedName = canonicalName;
        (void)GetSettings(resolvedName);
        const auto cached = settingsCache.find(Lowercase(canonicalName));
        if (cached == settingsCache.end())
        {
            a_error = "The profile settings cache is unavailable.";
            return false;
        }

        auto currentOverrides = cached->second.presetPreviewUserLayer;
        if (!currentOverrides) currentOverrides = CurrentUserLayer(*profile, a_error);
        const auto paths = ExpandSettingScopes(a_scopes);
        const auto settingsToSave = currentOverrides && !paths.empty() ?
                                        JsonOverlay::ProjectPaths(*currentOverrides, paths, a_error) :
                                        currentOverrides;
        const auto userDocument = settingsToSave ? Parse(*settingsToSave) : nullptr;
        auto* userRoot = userDocument ? yyjson_doc_get_root(userDocument.get()) : nullptr;
        if (!yyjson_is_obj(userRoot))
        {
            if (a_error.empty()) a_error = "The user settings could not be read.";
            return false;
        }
        if (yyjson_obj_size(userRoot) == 0)
        {
            a_error = a_scopes.empty() ?
                          "There are no user settings to make permanent." :
                          "There are no user settings on this page to make permanent.";
            return false;
        }

        const auto defaultsPath = ProfileDefaultsPath(*profile);
        const auto profileSettings = ReadText(defaultsPath).value_or(std::string(ProfileSettings::kNewBaseSettings));
        const auto merged = JsonOverlay::Overlay(profileSettings, *settingsToSave, a_error);
        if (!merged || !ParseSettings(*merged, defaultsPath))
        {
            if (a_error.empty()) a_error = "The profile settings could not be prepared.";
            return false;
        }
        if (!WriteTextAtomically(defaultsPath, JsonFormatting::Pretty(*merged), a_error)) return false;

        profile->defaultSettingRoots = SettingRoots(ProfileDefaultsText(*profile, a_error).value_or(*merged));
        const auto localDefaults = LocalDefaultsText(*profile, a_error);
        const auto presetDefaults = localDefaults ? PresetDefaultsText(*profile, a_error) : std::nullopt;
        if (!localDefaults || !presetDefaults)
        {
            a_error = "The saved profile settings could not be reloaded.";
            return false;
        }
        cached->second.localDefaults = *localDefaults;
        cached->second.presetDefaults = *presetDefaults;
        cached->second.preparedStack.reset();
        WeatherPatcher::InvalidatePresetCache();
        ApplySettings(std::format(
            "settings-saved-as-default | profile={} | scope={}",
            canonicalName,
            a_scopes.empty() ? "all" : "page"));
        a_profileName = canonicalName;
        return true;
    }

    bool SavePageSettings(std::string& a_profileName, const std::vector<std::string>& a_scopes)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaultText = profile ? PresetDefaultsText(*profile, error) : std::nullopt;
        if (!profile || !defaultText || !ParseSettings(*defaultText, ProfileDefaultsPath(*profile)))
        {
            return false;
        }

        const auto paths = ExpandSettingScopes(a_scopes);
        const auto current = SerializeProfileSettings(*profile, GetSettings(a_profileName));
        const auto allDifferences = JsonOverlay::Difference(current, *defaultText, error);
        const auto pageDifferences = allDifferences ? JsonOverlay::ProjectPaths(*allDifferences, paths, error) : std::nullopt;
        const auto existingText = ReadText(UserSettingsPath(*profile)).value_or("{}");
        const auto existing = UserSettingsValuesOnly(existingText, error);
        const auto retainedExisting = existing ? JsonOverlay::RemovePaths(*existing, paths, error) : std::nullopt;
        const auto difference = pageDifferences && retainedExisting ?
                                    JsonOverlay::Overlay(*retainedExisting, *pageDifferences, error) :
                                    std::nullopt;
        const auto userDifference = difference ?
                                        UserSettingsValuesOnly(*difference, error) :
                                        std::nullopt;
        if (!userDifference || !WriteUserSettings(*profile, *userDifference))
        {
            logger::warn("[TuningUtil] {} page save failed | {}", a_profileName, error);
            return false;
        }
        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            cached->second.explicitUserSettings = *userDifference;
            if (cached->second.presetPreviewUserLayer)
            {
                const auto currentUser = CurrentUserLayer(*profile, error);
                if (currentUser) cached->second.presetPreviewUserLayer = *currentUser;
            }
        }
        return true;
    }

    bool RestoreSettings(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto setup = profile ?
                               JsonOverlay::ProjectPaths(
                                   SerializeSettings(GetSettings(a_profileName)),
                                   ProfileSetup::kSettingPaths,
                                   error) :
                               std::nullopt;
        if (!profile || !setup)
        {
            return false;
        }
        settingsCache.erase(Lowercase(profile->name));
        auto& restoredSettings = GetSettings(a_profileName);
        const auto restored = JsonOverlay::Overlay(
            SerializeSettings(restoredSettings),
            *setup,
            error);
        auto parsed = restored ?
                          ParseSettings(*restored, ProfileDefaultsPath(*profile)) :
                          std::nullopt;
        if (!parsed) return false;
        restoredSettings = std::move(*parsed);
        ApplySettings(std::format("profile-restore | profile={}", a_profileName));
        return true;
    }

    bool RestorePageSettings(std::string& a_profileName, const std::vector<std::string>& a_scopes)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaultText = profile ? PresetDefaultsText(*profile, error) : std::nullopt;
        const auto stored = profile && defaultText ? StoredSettingsText(*profile, *defaultText, error) : std::nullopt;
        const auto paths = ExpandSettingScopes(a_scopes);
        const auto page = stored ? JsonOverlay::ProjectPaths(*stored, paths, error) : std::nullopt;
        if (!profile || !page)
        {
            return false;
        }
        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            const auto retained = JsonOverlay::RemovePaths(cached->second.explicitUserSettings, paths, error);
            const auto userText = ReadText(UserSettingsPath(*profile)).value_or("{}");
            const auto user = UserSettingsValuesOnly(userText, error);
            const auto restored = user ? JsonOverlay::ProjectPaths(*user, paths, error) : std::nullopt;
            const auto explicitSettings = retained && restored ? JsonOverlay::Overlay(*retained, *restored, error) : std::nullopt;
            if (explicitSettings) cached->second.explicitUserSettings = std::move(*explicitSettings);
        }
        return ApplySettingsPatch(*profile, a_profileName, *page, "page-restore");
    }

    bool ResetAllSettingsToDefault(std::string& a_profileName)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto setup = profile ?
                               JsonOverlay::ProjectPaths(
                                   SerializeSettings(GetSettings(a_profileName)),
                                   ProfileSetup::kSettingPaths,
                                   error) :
                               std::nullopt;
        const auto defaults = profile ? LocalDefaultsText(*profile, error) : std::nullopt;
        const auto reset = setup && defaults ?
                               JsonOverlay::Overlay(*defaults, *setup, error) :
                               std::nullopt;
        if (!profile || !defaults || !reset)
        {
            return false;
        }
        auto parsed = ParseProfileSettings(*reset, *profile);
        if (!parsed)
        {
            return false;
        }
        auto& cached = settingsCache[Lowercase(profile->name)];
        const auto previous = cached;
        cached.settings = std::move(*parsed);
        cached.localDefaults = *defaults;
        cached.presetDefaults = *defaults;
        cached.explicitUserSettings = "{}";
        cached.presetPreviewUserLayer.reset();
        if (WeatherPatcher::GetPresetCategories(a_profileName).empty())
        {
            WeatherPatcher::DiscardPresetPreview(a_profileName);
            ApplySettings(std::format("profile-reset-defaults | profile={}", a_profileName));
            return true;
        }
        if (!WeatherPatcher::PreviewPresetDefaults(a_profileName, error))
        {
            cached = previous;
            logger::warn("[TuningUtil] profile defaults reset failed | profile={} | {}", a_profileName, error);
            return false;
        }
        return true;
    }

    bool ResetSettingsToDefault(std::string& a_profileName, const std::vector<std::string>& a_scopes)
    {
        const auto* profile = FindProfile(a_profileName);
        std::string error;
        const auto defaults = profile ? LocalDefaultsText(*profile, error) : std::nullopt;
        const auto paths = ExpandSettingScopes(a_scopes);
        const auto page = defaults ? JsonOverlay::ProjectPaths(*defaults, paths, error) : std::nullopt;
        if (!profile || !page)
        {
            return false;
        }
        if (auto cached = settingsCache.find(Lowercase(profile->name)); cached != settingsCache.end())
        {
            if (auto retained = JsonOverlay::RemovePaths(cached->second.explicitUserSettings, paths, error))
            {
                cached->second.explicitUserSettings = std::move(*retained);
            }
        }
        return ApplySettingsPatch(*profile, a_profileName, *page, "page-reset-defaults");
    }


    namespace
    {
        void LogSettingsUpdate(const std::string_view a_trigger, const SettingsUpdate::Targets a_targets,
            const std::string_view a_rebuildProfile, const bool a_commitLightPlacer,
            const std::source_location a_source)
        {
            if (!DetailedLogging::IsEnabled()) return;
            const std::string_view sourceFile = a_source.file_name();
            const auto filename = sourceFile.substr(sourceFile.find_last_of("/\\") + 1);
            DetailedLogging::Info(
                "[Settings Update] trigger={} | caller={}:{} | targets={} | rebuildProfile={} | commitLightPlacer={} | status={}",
                a_trigger, filename, a_source.line(), SettingsUpdate::Describe(a_targets), a_rebuildProfile, a_commitLightPlacer,
                runtimeStateReleased ? "skipped-runtime-released" : "apply");
        }
    }

    void ApplySettings(const std::string_view a_trigger, const bool a_commitLightPlacer,
        const std::source_location a_source)
    {
        ApplyProfileSettings({}, SettingsUpdate::Targets::all, a_trigger, a_commitLightPlacer, a_source);
    }

    void ApplyProfileSettings(const std::string_view a_profile, const SettingsUpdate::Targets a_targets,
        const std::string_view a_trigger, const bool a_commitLightPlacer, const std::source_location a_source)
    {
        LogSettingsUpdate(a_trigger, a_targets, a_profile.empty() ? "all" : a_profile, a_commitLightPlacer, a_source);
        if (runtimeStateReleased)
        {
            return;
        }
        for (const auto& profile : profiles)
        {
            if (!a_profile.empty() && !Config::IEquals(profile.name, a_profile)) continue;
            const auto cached = settingsCache.find(Lowercase(profile.name));
            if (cached == settingsCache.end()) continue;
            cached->second.preparedStack.reset();
            if (auto expanded = ParseProfileSettings(SerializeSettings(cached->second.settings), profile))
                cached->second.settings = std::move(*expanded);
        }
        ++settingsRevision;
        if (a_profile.empty()) SynchronizeProfilePriorities();
        using enum SettingsUpdate::Targets;
        if (SettingsUpdate::Contains(a_targets, weather)) WeatherPatcher::ApplyAllSettings();
        if (SettingsUpdate::Contains(a_targets, lighting)) LightingPatcher::ApplyTemplateSettings();
        if (SettingsUpdate::Contains(a_targets, pointLights)) LightingPatcher::ApplyPointLightSettings(a_commitLightPlacer);
        if (SettingsUpdate::Contains(a_targets, objects)) ObjectLightingPatcher::ApplyAllSettings();
        if (SettingsUpdate::Contains(a_targets, imageSpaces)) ImageSpacePatcher::ApplyFilmicCurveWhitePoint();
    }

    void CommitLightPlacerSettings(const std::string_view a_trigger, const std::source_location a_source)
    {
        LogSettingsUpdate(a_trigger, SettingsUpdate::Targets::pointLights, "none", true, a_source);
        if (!runtimeStateReleased) LightingPatcher::ApplyPointLightSettings(true);
    }

    std::uint64_t GetSettingsRevision()
    {
        return settingsRevision;
    }

    std::uint64_t GetSliderBindingsRevision()
    {
        return sliderBindingsRevision;
    }

    void ApplyDataLoaded()
    {
        DetailedLogging::Info("[Settings Update] trigger={} | status=apply", "data-loaded");
        ++settingsRevision;
        runtimeStateReleased = false;
        startupSettingTargetOverlapCache.clear();
        startupSettingTargetOverlapsCaptured = false;
        pluginDependencyFilterReady = true;
        InvalidateDiscoveryCaches();
        (void)GetProfiles();
        for (auto& profile : profiles)
        {
            SanitizeStoredUserSettings(profile);
            auto name = profile.name;
            (void)GetSettings(name);
        }
        SynchronizeProfilePriorities();
        WeatherPatcher::ApplyDataLoaded();
        LightingPatcher::ApplyDataLoaded();
        ObjectLightingPatcher::ApplyAllSettings();
        if (TuningSettings::IsTuningMenuEnabledForSession())
        {
            CaptureStartupSettingTargetOverlaps();
        }
        CSTonemapping::Initialize();
        ImageSpacePatcher::ApplyFilmicCurveWhitePoint();
        if (!TuningSettings::IsTuningMenuEnabledForSession())
        {
            WeatherPatcher::ReleaseRuntimeState();
            LightingPatcher::ReleaseRuntimeState();
            ImageSpacePatcher::ReleaseRuntimeState();
            CSTonemapping::ReleaseRuntimeState();
            ++sliderBindingsRevision;
            profiles = {};
            settingsCache = {};
            globalDefaultsCache.reset();
            discoveryInitialized = false;
            runtimeStateReleased = true;
            logger::info("[TuningUtil] startupOnly=true | pointLightState=retained");
        }
    }
}  // namespace MPL::TuningUtil
