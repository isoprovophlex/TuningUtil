#include <CSTonemapping.h>
#include <Config.h>
#include <DetailedLogging.h>
#include <HeliosphanAPI.h>
#include <ImageSpacePatcher.h>
#include <TuningUtil.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace MPL::ImageSpacePatcher
{
    namespace
    {
        using ImageSpaceSet = std::unordered_set<RE::TESImageSpace*>;
        using SettingsMap = std::unordered_map<RE::TESImageSpace*, WeatherPatcher::ImageSpaceSettings>;

        struct InteriorImageSpaceCache
        {
            bool initialized = false;
            ImageSpaceSet imageSpaces;
        };

        struct RuntimeMonitorCache
        {
            std::mutex lock;
            RuntimeMonitor value;
            std::chrono::steady_clock::time_point nextRefresh{};
            bool refreshPending = false;
        };

        InteriorImageSpaceCache& GetInteriorImageSpaceCache()
        {
            static InteriorImageSpaceCache cache;
            return cache;
        }

        RuntimeMonitorCache& GetRuntimeMonitorCache()
        {
            static RuntimeMonitorCache cache;
            return cache;
        }

        const HeliosphanAPI::Interface* GetHeliosphanAPI()
        {
            static const auto* api = []
            {
                const auto module = GetModuleHandleW(L"Heliosphan.dll");
                const auto request = module ?
                                         reinterpret_cast<HeliosphanAPI::RequestInterface>(
                                             GetProcAddress(module, "Heliosphan_RequestAPI")) :
                                         nullptr;
                const auto* result = request ? request(HeliosphanAPI::kVersion) : nullptr;
                return result && result->version == HeliosphanAPI::kVersion &&
                               result->IsAutoCSTonemappingApplied &&
                               result->SetAutoCSTonemappingSuppressed ?
                           result :
                           nullptr;
            }();
            return api;
        }

        RuntimeMonitor CaptureRuntimeMonitor()
        {
            RuntimeMonitor result;
            if (const auto* setting = RE::GetINISetting("bUseFilmicCurve:Display"))
            {
                result.filmicCurve = setting->GetBool();
                result.filmicCurveAvailable = true;
            }
            if (const auto* setting = RE::GetINISetting("fFilmicWhiteScale:Display"))
            {
                result.filmicWhiteScale = setting->GetFloat();
                result.filmicWhiteScaleAvailable = true;
            }
            return result;
        }

        bool PluginFilterEmpty(const TuningUtil::PluginFilter& a_filter)
        {
            return a_filter.exact.empty() && a_filter.contains.empty();
        }

        bool PluginNameMatches(
            const std::string_view a_pluginName,
            const TuningUtil::PluginFilter& a_filter)
        {
            if (std::ranges::any_of(a_filter.exact, [&](const auto& a_name)
                    { return Config::IEquals(a_pluginName, a_name); }))
            {
                return true;
            }
            auto pluginName = std::string(a_pluginName);
            std::ranges::transform(pluginName, pluginName.begin(), [](const unsigned char a_character)
                { return static_cast<char>(std::tolower(a_character)); });
            return std::ranges::any_of(a_filter.contains, [&](auto a_fragment)
            {
                std::ranges::transform(a_fragment, a_fragment.begin(), [](const unsigned char a_character)
                    { return static_cast<char>(std::tolower(a_character)); });
                return !a_fragment.empty() && pluginName.contains(a_fragment);
            });
        }

        bool MatchesPluginFilter(
            const RE::TESImageSpace* a_imageSpace,
            const TuningUtil::PluginFilter& a_filter)
        {
            if (!a_imageSpace || PluginFilterEmpty(a_filter))
            {
                return false;
            }
            if (a_imageSpace->sourceFiles.array)
            {
                for (const auto* sourceFile : *a_imageSpace->sourceFiles.array)
                {
                    if (sourceFile && PluginNameMatches(sourceFile->GetFilename(), a_filter))
                    {
                        return true;
                    }
                }
            }
            const auto* winningFile = a_imageSpace->GetFile();
            return winningFile && PluginNameMatches(winningFile->GetFilename(), a_filter);
        }

        void ApplyMultipliers(RE::TESImageSpace* a_imageSpace, const WeatherPatcher::ImageSpaceSettings& a_settings)
        {
            if (!a_imageSpace)
            {
                return;
            }

            const auto apply = [](float& a_value, const double a_multiplier)
            {
                const auto multiplier = std::max(0.0, a_multiplier);
                if (std::abs(multiplier - 1.0) > 0.0001)
                {
                    a_value *= static_cast<float>(multiplier);
                }
            };
            apply(a_imageSpace->data.cinematic.saturation, a_settings.saturationMultiplier);
            apply(a_imageSpace->data.cinematic.brightness, a_settings.brightnessMultiplier);
            apply(a_imageSpace->data.cinematic.contrast, a_settings.contrastMultiplier);
            apply(a_imageSpace->data.hdr.sunlightScale, a_settings.sunlightScaleMultiplier);
            apply(a_imageSpace->data.hdr.skyScale, a_settings.skyScaleMultiplier);
        }

        void CopyAdjustedFields(RE::ImageSpaceBaseData& a_target, const RE::ImageSpaceBaseData& a_source)
        {
            a_target.cinematic.saturation = a_source.cinematic.saturation;
            a_target.cinematic.brightness = a_source.cinematic.brightness;
            a_target.cinematic.contrast = a_source.cinematic.contrast;
            a_target.hdr.sunlightScale = a_source.hdr.sunlightScale;
            a_target.hdr.skyScale = a_source.hdr.skyScale;
        }

        void SynchronizeCurrentInteriorImageSpace(const SettingsMap& a_settings)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell = player ? player->GetParentCell() : nullptr;
            if (!cell || !cell->IsInteriorCell())
            {
                return;
            }

            const auto* extra = cell->extraList.GetByType<RE::ExtraCellImageSpace>();
            auto* imageSpace = extra ? extra->imageSpace : nullptr;
            if (!imageSpace || !a_settings.contains(imageSpace))
            {
                return;
            }

            auto* manager = RE::ImageSpaceManager::GetSingleton();
            if (!manager)
            {
                return;
            }

            const auto synchronize = [&](auto& a_runtime)
            {
                const auto* stat = Config::StatData::GetSingleton();
                const auto currentBaseIsRecord = std::ranges::any_of(
                    stat->imageSpaceBaselines,
                    [&](const auto& a_entry) { return &a_entry.first->data == a_runtime.currentBaseData; });
                if (a_runtime.currentBaseData && !currentBaseIsRecord)
                {
                    CopyAdjustedFields(*a_runtime.currentBaseData, imageSpace->data);
                }
                CopyAdjustedFields(a_runtime.data.baseData, imageSpace->data);
            };
            if (REL::Module::IsVR()) synchronize(manager->GetVRRuntimeData());
            else synchronize(manager->GetRuntimeData());
        }

        const ImageSpaceSet& GetInteriorImageSpaces()
        {
            auto& cache = GetInteriorImageSpaceCache();
            if (cache.initialized)
            {
                return cache.imageSpaces;
            }
            cache.initialized = true;

            std::size_t interiorCells = 0;
            std::size_t imageSpaceReferences = 0;
            const auto& [forms, lock] = RE::TESForm::GetAllForms();
            const RE::BSReadLockGuard guard{ lock };
            if (!forms)
            {
                logger::warn("[Image Space] classification failed | loaded form map unavailable");
                return cache.imageSpaces;
            }

            for (const auto& [formID, form] : *forms)
            {
                (void)formID;
                auto* cell = form ? form->As<RE::TESObjectCELL>() : nullptr;
                if (!cell || !cell->IsInteriorCell())
                {
                    continue;
                }
                ++interiorCells;
                const auto* extra = cell->extraList.GetByType<RE::ExtraCellImageSpace>();
                if (extra && extra->imageSpace)
                {
                    cache.imageSpaces.insert(extra->imageSpace);
                    ++imageSpaceReferences;
                }
            }

            logger::info(
                "[Image Space] interior classification | records={} | references={} | cells={}",
                cache.imageSpaces.size(),
                imageSpaceReferences,
                interiorCells);
            return cache.imageSpaces;
        }

    }  // namespace

    void ApplyFilmicCurveWhitePoint()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("[Image Space] apply failed | TESDataHandler unavailable");
            return;
        }

        auto* stat = Config::StatData::GetSingleton();
        for (auto* imageSpace : dataHandler->GetFormArray<RE::TESImageSpace>())
        {
            if (!imageSpace)
            {
                continue;
            }
            const auto baseline = stat->imageSpaceBaselines.try_emplace(imageSpace, imageSpace->data).first;
            const auto white = imageSpace->data.hdr.white;
            imageSpace->data = baseline->second;
            imageSpace->data.hdr.white = white;
        }

        const auto& intImageSpaces = GetInteriorImageSpaces();
        SettingsMap interiorSettings;
        SettingsMap exteriorSettings;
        ImageSpaceSet explicitWhiteTargets;

        static constexpr std::array interiorRoots{ std::string_view{ "intImageSpace" } };
        std::vector<std::string> activeLightingProfiles;
        for (auto profileName : TuningUtil::GetProfilesWithSettings(interiorRoots))
        {
            const auto& profileSettings = TuningUtil::GetSettings(profileName);
            if (profileSettings.EnableProfile)
            {
                activeLightingProfiles.push_back(std::move(profileName));
            }
        }
        if (!activeLightingProfiles.empty())
        {
            const auto settings = TuningUtil::ResolveSettingsStack(activeLightingProfiles);
            const auto& category = settings.intImageSpace;
            for (auto* imageSpace : intImageSpaces)
            {
                interiorSettings[imageSpace] = category;
            }
            DetailedLogging::Info(
                "[Image Space] interior | profiles={} | targets={}",
                activeLightingProfiles.size(),
                intImageSpaces.size());
        }

        struct ActiveWeatherProfile
        {
            std::string profileName;
            TuningUtil::PluginFilter inclusions;
            TuningUtil::PluginFilter exclusions;
            bool catchAll;
        };

        std::vector<ActiveWeatherProfile> activeWeatherProfiles;
        static constexpr std::array exteriorRoots{ std::string_view{ "exteriorImageSpace" } };
        for (auto& profileName : TuningUtil::GetProfilesWithSettings(exteriorRoots))
        {
            const auto& settings = TuningUtil::GetSettings(profileName);
            if (const auto* api = GetHeliosphanAPI())
            {
                const auto force = settings.exteriorImageSpace.ForceCSTonemapping;
                api->SetAutoCSTonemappingSuppressed(
                    profileName.c_str(),
                    settings.EnableProfile && force.has_value() && !*force);
            }
            if (!settings.EnableProfile)
            {
                continue;
            }
            const auto catchAll = PluginFilterEmpty(settings.pluginInclusions);
            activeWeatherProfiles.push_back({
                profileName,
                settings.pluginInclusions,
                settings.pluginExclusions,
                catchAll,
            });
        }

        std::unordered_map<std::string, std::size_t> profileTargetCounts;
        std::unordered_map<std::string, TuningUtil::Settings> resolvedStacks;
        for (auto* imageSpace : dataHandler->GetFormArray<RE::TESImageSpace>())
        {
            if (!imageSpace || intImageSpaces.contains(imageSpace))
            {
                continue;
            }
            const auto claimed = std::ranges::any_of(activeWeatherProfiles, [&](const auto& a_profile)
            {
                return !a_profile.catchAll &&
                       !MatchesPluginFilter(imageSpace, a_profile.exclusions) &&
                       MatchesPluginFilter(imageSpace, a_profile.inclusions);
            });
            std::vector<std::string> matchingProfiles;
            std::string signature;
            for (const auto& profile : activeWeatherProfiles)
            {
                const auto excluded = MatchesPluginFilter(imageSpace, profile.exclusions);
                const auto targeted = !excluded &&
                                      (profile.catchAll ? !claimed :
                                                          MatchesPluginFilter(imageSpace, profile.inclusions));
                if (targeted)
                {
                    matchingProfiles.push_back(profile.profileName);
                    signature.append(profile.profileName).push_back('\x1F');
                    ++profileTargetCounts[profile.profileName];
                }
            }
            if (matchingProfiles.empty())
            {
                continue;
            }
            const auto [stack, inserted] = resolvedStacks.try_emplace(signature);
            if (inserted)
            {
                stack->second = TuningUtil::ResolveSettingsStack(matchingProfiles);
            }
            const auto& settings = stack->second;
            exteriorSettings[imageSpace] = settings.exteriorImageSpace;
        }
        for (const auto& profile : activeWeatherProfiles)
        {
            DetailedLogging::Info(
                "[Image Space] exterior | profile={} | targets={} | scope={}",
                profile.profileName,
                profileTargetCounts[profile.profileName],
                profile.catchAll ? "unclaimed" : "plugin");
        }

        const auto applySettings = [&](const SettingsMap& a_settings)
        {
            for (const auto& [imageSpace, settings] : a_settings)
            {
                ApplyMultipliers(imageSpace, settings);
                if (settings.ForceCSTonemapping.value_or(false))
                {
                    explicitWhiteTargets.insert(imageSpace);
                }
            }
        };
        applySettings(interiorSettings);
        applySettings(exteriorSettings);
        SynchronizeCurrentInteriorImageSpace(interiorSettings);

        CSTonemapping::SetForcedTargets(explicitWhiteTargets);

        logger::info(
            "[Image Space] apply | interior={} | exterior={} | forcedTonemapping={}",
            intImageSpaces.size(),
            exteriorSettings.size(),
            explicitWhiteTargets.size());
    }

    std::optional<bool> IsAutoCSTonemappingApplied(const std::string_view a_profile)
    {
        const auto* api = GetHeliosphanAPI();
        if (!api)
        {
            return std::nullopt;
        }
        const std::string profile(a_profile);
        return api->IsAutoCSTonemappingApplied(profile.c_str());
    }

    void RequestRuntimeMonitorRefresh()
    {
        using namespace std::chrono_literals;

        auto& cache = GetRuntimeMonitorCache();
        const auto now = std::chrono::steady_clock::now();
        {
            const std::scoped_lock lock(cache.lock);
            if (cache.refreshPending || now < cache.nextRefresh)
            {
                return;
            }
            cache.refreshPending = true;
            cache.nextRefresh = now + 1s;
        }

        if (const auto* tasks = SKSE::GetTaskInterface())
        {
            tasks->AddTask([]
            {
                auto value = CaptureRuntimeMonitor();
                auto& taskCache = GetRuntimeMonitorCache();
                const std::scoped_lock lock(taskCache.lock);
                taskCache.value = value;
                taskCache.refreshPending = false;
            });
            return;
        }

        {
            const std::scoped_lock lock(cache.lock);
            cache.refreshPending = false;
        }
    }

    RuntimeMonitor ReadRuntimeMonitor()
    {
        auto& cache = GetRuntimeMonitorCache();
        const std::scoped_lock lock(cache.lock);
        return cache.value;
    }

    void ReleaseRuntimeState()
    {
        GetInteriorImageSpaceCache() = {};
        auto& monitor = GetRuntimeMonitorCache();
        {
            const std::scoped_lock lock(monitor.lock);
            monitor.value = {};
            monitor.nextRefresh = {};
            monitor.refreshPending = false;
        }
        auto* stat = Config::StatData::GetSingleton();
        stat->imageSpaceBaselines = {};
    }
}  // namespace MPL::ImageSpacePatcher
