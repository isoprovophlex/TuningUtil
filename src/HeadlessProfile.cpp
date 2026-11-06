#include <HeadlessProfile.h>
#include <Config/Common.h>
#include <JsonOverlay.h>
#include <ProfileParsing.h>
#include <ProfileSettings.h>
#ifdef TUNING_HELIOS
#include <WeatherPatcher.h>
#endif
#include <fstream>

namespace MPL::TuningUtil
{
    namespace
    {
        using namespace ProfileParsing;
        std::vector<Profile> profiles;
        Settings settings;

        const Profile* FindProfile(const std::string_view a_name)
        {
            return !profiles.empty() && Config::IEquals(profiles.front().name, a_name) ?
                       &profiles.front() : nullptr;
        }

        std::optional<std::string> ReadText(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file) return std::nullopt;
            std::string text(std::istreambuf_iterator<char>(file), {});
            if (file.bad()) return std::nullopt;
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom)) text.erase(0, bom.size());
            return text;
        }

        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const { yyjson_doc_free(a_document); }
        };
        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;

        bool OwnsAnySetting(const Profile& a_profile, const std::span<const std::string_view> a_roots)
        {
            for (const auto root : a_roots)
            {
#ifdef TUNING_ATLM
                if (root == "normalizeLuminance" && settings.normalizeLuminance) return true;
#endif
                if (std::ranges::contains(a_profile.defaultSettingRoots, root)) return true;
                for (const auto& binding : a_profile.sliderBindings)
                    if (binding.ruleValueMap.empty())
                        for (const auto& target : binding.targets)
                            if (target.path == root || target.path.starts_with(std::string(root) + ".")) return true;
            }
            return false;
        }
    }

    bool LoadHeadlessProfile(const std::string_view a_provider)
    {
        const std::filesystem::path root{ "./Data/Luma/Tuning" };
        const auto directory = root / a_provider;
        const auto defaultsPath = directory / "profileSettings.json";
        std::string error;
        const auto loadedDefaults = ProfileSettings::Load(directory, error);
        const auto defaults = loadedDefaults ? std::optional{ loadedDefaults->text } : std::nullopt;
        auto parsed = defaults ? ProfileParsing::ParseSettings(*defaults, defaultsPath) : std::nullopt;
        if (!parsed)
        {
            logger::error("[{}Tuning] profile unavailable | path={} | {}", a_provider, defaultsPath.string(), error);
            return false;
        }
        const Document document(yyjson_read(defaults->data(), defaults->size(), 0));
        auto* object = document ? yyjson_doc_get_root(document.get()) : nullptr;
        const auto dependencies = JsonStrings(object, "PluginDependency");
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dependencies.empty() && (!dataHandler ||
            !std::ranges::any_of(dependencies, [&](const auto& a_plugin)
            {
                return dataHandler->LookupLoadedModByName(a_plugin) ||
                       dataHandler->LookupLoadedLightModByName(a_plugin);
            })))
        {
            logger::info("[{}Tuning] inactive | plugin dependency missing", a_provider);
            return false;
        }

        Profile profile{ .priority = parsed->profilePriority, .directory = directory };
        std::size_t index, count;
        yyjson_val* key;
        yyjson_val* value;
        yyjson_obj_foreach(object, index, count, key, value)
            profile.defaultSettingRoots.emplace_back(yyjson_get_str(key), yyjson_get_len(key));

        const auto layoutPath = directory / "skseMenu.json";
        if (const auto text = ReadText(layoutPath))
        {
            auto layout = ProfileParsing::ParseLayout(*text, layoutPath, error);
            if (!layout)
            {
                logger::error("[{}Tuning] profile layout rejected | {}", a_provider, error);
                return false;
            }
            profile.name = std::move(layout->profile);
            profile.sliderBindings = std::move(layout->bindings);
#if defined(TUNING_ATLM)
            profile.filteredLightingTemplateRules = std::move(layout->lightingRules);
            profile.lightingSliderSettings = std::move(layout->lightingLinks.settings);
            profile.customLightingSliderLinks = std::move(layout->lightingLinks.customLinks);
#elif defined(TUNING_HELIOS)
            profile.filteredWeatherRules = std::move(layout->weatherRules);
#endif
        }
        else if (!parsed->sliderValues.empty())
        {
            logger::error("[{}Tuning] profile layout required for sliderValues | path={}", a_provider, layoutPath.string());
            return false;
        }
        else
        {
            std::error_code fileError;
            if (std::filesystem::exists(layoutPath, fileError) || fileError)
            {
                logger::error("[{}Tuning] profile layout unreadable | path={}", a_provider, layoutPath.string());
                return false;
            }
        }

        if (profile.name.empty()) profile.name = JsonString(object, "profile").value_or("");
        if (!Config::IEquals(profile.name, a_provider))
        {
            logger::error("[{}Tuning] profile rejected | expected provider name", a_provider);
            return false;
        }

        auto combined = *defaults;
        const auto globalPath = root / "defaultSettings.json";
        if (const auto global = ReadText(globalPath); global && ProfileParsing::ParseSettings(*global, globalPath))
        {
            const auto overlay = JsonOverlay::Overlay(*global, combined, error);
            if (!overlay) return false;
            combined = *overlay;
        }
        const auto stored = SliderStorage::Store(combined, profile.sliderBindings, true, error);
        const auto expanded = stored ? SliderStorage::Materialize(*stored, profile.sliderBindings, error) : std::nullopt;
        parsed = expanded ? ProfileParsing::ParseSettings(*expanded, defaultsPath) : std::nullopt;
        if (!parsed)
        {
            logger::error("[{}Tuning] profile values rejected | {}", a_provider, error);
            return false;
        }
        settings = std::move(*parsed);
#ifdef TUNING_HELIOS
        if (settings.EnableProfile)
        {
            profile.ambientAnchorWeather = Trim(JsonString(object, "ambientAnchorWeather").value_or("SkyrimClear"));
            if (profile.ambientAnchorWeather.empty()) profile.ambientAnchorWeather = "SkyrimClear";
            profile.runtimeCompressionAnchors = WeatherPatcher::ReadWeatherCompressionAnchors(profile.ambientAnchorWeather);
            if (!profile.runtimeCompressionAnchors && !Config::IEquals(profile.ambientAnchorWeather, "SkyrimClear"))
                profile.runtimeCompressionAnchors = WeatherPatcher::ReadWeatherCompressionAnchors("SkyrimClear");
        }
#endif
        profiles.push_back(std::move(profile));
        logger::info("[{}Tuning] profile loaded | enabled={} | bindings={} | user state=ignored",
            a_provider, settings.EnableProfile, profiles.front().sliderBindings.size());
        return true;
    }

    const std::vector<Profile>& GetProfiles() { return profiles; }
    std::uint64_t GetSettingsRevision() { return 1; }
    std::uint64_t GetSliderBindingsRevision() { return 1; }

    Settings& GetSettings(std::string& a_profile)
    {
        assert(FindProfile(a_profile));
        return settings;
    }

    Settings ResolveSettingsStack(const std::span<const std::string> a_profiles)
    {
        return std::ranges::any_of(a_profiles, [](const auto& a_name) { return FindProfile(a_name); }) ?
                   settings : Settings{};
    }

    std::vector<std::string> GetProfilesWithSettings(const std::span<const std::string_view> a_roots)
    {
        return !profiles.empty() && OwnsAnySetting(profiles.front(), a_roots) ?
                   std::vector{ profiles.front().name } : std::vector<std::string>{};
    }

    std::optional<std::string> GetExclusiveController(const ControlDomain a_domain)
    {
        if (profiles.empty() || !settings.EnableProfile) return std::nullopt;
        const auto& control = settings.exclusiveControl;
        bool claimed = false;
        switch (a_domain)
        {
        case ControlDomain::weather: claimed = control.weather; break;
        case ControlDomain::exteriorImageSpace: claimed = control.exteriorImageSpace; break;
        case ControlDomain::lightingTemplates: claimed = control.lightingTemplates; break;
        case ControlDomain::cellLighting: claimed = control.cellLighting; break;
        case ControlDomain::lightingImageSpace: claimed = control.lightingImageSpace; break;
        }
        return claimed ? std::optional{ profiles.front().name } : std::nullopt;
    }

#ifdef TUNING_HELIOS
    const std::vector<FilteredWeatherRule>& GetFilteredWeatherRules(const std::string& a_name)
    {
        assert(FindProfile(a_name));
        return profiles.front().filteredWeatherRules;
    }

    WeatherPatcher::WeatherCompressionAnchors ResolveCompressionAnchors(const std::span<const std::string> a_names)
    {
        return !a_names.empty() && !profiles.empty() && profiles.front().runtimeCompressionAnchors ?
                   *profiles.front().runtimeCompressionAnchors : WeatherPatcher::WeatherCompressionAnchors{};
    }
#endif

#ifdef TUNING_ATLM
    const std::vector<FilteredLightingTemplateRule>& GetFilteredLightingTemplateRules(const std::string& a_name)
    {
        assert(FindProfile(a_name));
        return profiles.front().filteredLightingTemplateRules;
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


#endif
}
