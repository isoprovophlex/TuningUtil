#include <DetailedLogging.h>
#include <PointLightPatcher.h>
#include <TuningSettings.h>
#include <WeatherPatcher.h>
#include <XEMI_API.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <yyjson.h>

namespace MPL::PointLightPatcher
{
    namespace
    {
        using Settings = LightingPatcher::PointLightSettings;
        using EmittanceMap = std::unordered_map<std::string, std::string>;

        const std::filesystem::path kLightPlacerRoot{ "./Data/LightPlacer" };
        const std::filesystem::path kEmittanceMappingRoot{ "./Data/SKSE/XEMIUtil/LightPlacer" };
        constexpr RE::FormID kPointLightColorDiagnosticReference = 0x0003759B;
        constexpr bool kUseDirectLightPlacerNiLights = true;
        constexpr std::string_view kLightPlacerNodePrefix = "LP_Light[";
        constexpr std::uint8_t kPostReloadRefreshAttempts = 12;
        constexpr std::chrono::milliseconds kPostReloadRefreshDelay{ 100 };

        struct Baseline
        {
            float fade;
            RE::Color color;
        };

        struct ColorTuning
        {
            double saturationMultiplier;
            WeatherPatcher::AmbientHueScaleValues hueScales;
            WeatherPatcher::HueShiftBands hueShift;
            WeatherPatcher::HueRanges hueRanges;
        };

        struct AppliedState
        {
            Settings settings;
            WeatherPatcher::HueRanges hueRanges;

            bool operator==(const AppliedState&) const = default;
        };

        struct LightPlacerRuntimeBaseline
        {
            std::string name;
            RE::NiColor sourceDiffuse;
            RE::NiColor appliedDiffuse;
            bool hasApplied = false;
        };

        struct DocumentDeleter
        {
            void operator()(yyjson_doc* a_document) const { yyjson_doc_free(a_document); }
        };

        struct MutableDocumentDeleter
        {
            void operator()(yyjson_mut_doc* a_document) const { yyjson_mut_doc_free(a_document); }
        };

        using Document = std::unique_ptr<yyjson_doc, DocumentDeleter>;
        using MutableDocument = std::unique_ptr<yyjson_mut_doc, MutableDocumentDeleter>;

        std::unordered_map<RE::TESObjectLIGH*, Baseline> baselines;
        std::unordered_map<RE::TESObjectREFR*, float> referenceFadeBaselines;
        std::unordered_map<RE::NiPointLight*, LightPlacerRuntimeBaseline> lightPlacerRuntimeBaselines;
        std::unordered_set<RE::TESObjectLIGH*> externallyEmissiveLights;
        std::optional<AppliedState> appliedState;
        std::optional<AppliedState> directLightPlacerState;
        std::optional<double> referenceFadeMultiplier;
        std::optional<AppliedState> lightPlacerState;
        std::atomic<std::uint64_t> reloadGeneration{ 0 };
        std::atomic<std::uint64_t> directRefreshGeneration{ 0 };
        std::mutex brokerStateLock;
        std::optional<AppliedState> brokerTransformState;
        const XEMIAPI::Interface* xemiAPI = nullptr;
        bool brokerConnectionAttempted = false;
        bool runtimeEventsInstalled = false;
        std::atomic_bool brokerReloadPending{ false };

        void QueueDirectLightPlacerRefresh();
        void QueuePostReloadDirectLightPlacerRefresh();

        std::string Lowercase(std::string a_value)
        {
            std::ranges::transform(a_value, a_value.begin(), [](const unsigned char a_character)
                { return static_cast<char>(std::tolower(a_character)); });
            return a_value;
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

        std::vector<std::string> SplitLightEditorIDs(const std::string_view a_value)
        {
            std::vector<std::string> result;
            std::size_t start = 0;
            while (start <= a_value.size())
            {
                const auto end = a_value.find('|', start);
                result.push_back(Trim(std::string(a_value.substr(start, end - start))));
                if (end == std::string_view::npos)
                {
                    break;
                }
                start = end + 1;
            }
            return result;
        }

        RE::TESObjectLIGH* GetBaseLight(RE::TESObjectREFR* a_reference)
        {
            auto* baseObject = a_reference ? a_reference->GetBaseObject() : nullptr;
            return baseObject && baseObject->Is(RE::FormType::Light) ?
                       static_cast<RE::TESObjectLIGH*>(baseObject) :
                       nullptr;
        }

        bool HasExternalEmittance(const RE::TESObjectREFR* a_reference)
        {
            const auto* extra = a_reference ? a_reference->extraList.GetByType<RE::ExtraEmittanceSource>() : nullptr;
            return extra && extra->source;
        }

        std::optional<std::string> ReadFile(const std::filesystem::path& a_path)
        {
            std::ifstream file(a_path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        bool WriteFile(const std::filesystem::path& a_path, const std::string_view a_contents)
        {
            std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
            file.write(a_contents.data(), static_cast<std::streamsize>(a_contents.size()));
            file.flush();
            return static_cast<bool>(file);
        }

        bool KeyEquals(yyjson_val* a_key, const std::string_view a_expected)
        {
            return yyjson_is_str(a_key) && yyjson_get_len(a_key) == a_expected.size() &&
                   std::memcmp(yyjson_get_str(a_key), a_expected.data(), a_expected.size()) == 0;
        }

        EmittanceMap LoadEmittanceMappings()
        {
            EmittanceMap result;
            std::vector<std::filesystem::path> files;
            std::error_code error;
            if (!std::filesystem::is_directory(kEmittanceMappingRoot, error))
            {
                return result;
            }
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator iterator(kEmittanceMappingRoot, options, error), end;
                iterator != end && !error;
                iterator.increment(error))
            {
                if (iterator->is_regular_file(error) && Lowercase(iterator->path().extension().string()) == ".json")
                {
                    files.push_back(iterator->path());
                }
            }
            std::ranges::sort(files, [](const auto& a_left, const auto& a_right)
                { return Lowercase(a_left.generic_string()) < Lowercase(a_right.generic_string()); });

            for (const auto& path : files)
            {
                const auto text = ReadFile(path);
                if (!text)
                {
                    logger::warn("Could not read XEMIUtil Light Placer mapping {}", path.string());
                    continue;
                }
                yyjson_read_err readError{};
                Document document(yyjson_read_opts(
                    const_cast<char*>(text->data()), text->size(), YYJSON_READ_NOFLAG, nullptr, &readError));
                auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
                if (!yyjson_is_arr(root))
                {
                    logger::warn("Could not parse XEMIUtil Light Placer mapping {}", path.string());
                    continue;
                }
                std::size_t entryIndex = 0;
                std::size_t entryCount = 0;
                yyjson_val* entry = nullptr;
                yyjson_arr_foreach(root, entryIndex, entryCount, entry)
                {
                    auto* lights = yyjson_obj_get(entry, "light");
                    auto* emittance = yyjson_obj_get(entry, "externalEmittance");
                    if (!yyjson_is_arr(lights) || !yyjson_is_str(emittance))
                    {
                        continue;
                    }
                    const std::string emittanceName(yyjson_get_str(emittance), yyjson_get_len(emittance));
                    std::size_t lightIndex = 0;
                    std::size_t lightCount = 0;
                    yyjson_val* light = nullptr;
                    yyjson_arr_foreach(lights, lightIndex, lightCount, light)
                    {
                        if (yyjson_is_str(light))
                        {
                            result[Lowercase(std::string(yyjson_get_str(light), yyjson_get_len(light)))] = emittanceName;
                        }
                    }
                }
            }
            DetailedLogging::Info("Loaded {} XEMIUtil Light Placer emittance mapping(s)", result.size());
            return result;
        }

        std::optional<std::string> FindMappedEmittance(const EmittanceMap& a_mappings, const std::string_view a_editorIDs)
        {
            for (const auto& editorID : SplitLightEditorIDs(a_editorIDs))
            {
                if (const auto mapping = a_mappings.find(Lowercase(editorID)); mapping != a_mappings.end())
                {
                    return mapping->second;
                }
            }
            return std::nullopt;
        }

        yyjson_mut_val* CopyValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value,
            const double a_fadeMultiplier,
            const ColorTuning& a_tuning);

        yyjson_mut_val* CopyFadeControllerValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value,
            const double a_fadeMultiplier)
        {
            if (yyjson_is_obj(a_value))
            {
                auto* result = yyjson_mut_obj(a_document);
                yyjson_obj_iter iterator = yyjson_obj_iter_with(a_value);
                while (auto* key = yyjson_obj_iter_next(&iterator))
                {
                    auto* value = yyjson_obj_iter_get_val(key);
                    auto* copiedKey = yyjson_mut_strncpy(a_document, yyjson_get_str(key), yyjson_get_len(key));
                    yyjson_mut_val* copiedValue = nullptr;
                    if ((KeyEquals(key, "value") ||
                         KeyEquals(key, "forward") ||
                         KeyEquals(key, "backward")) &&
                        yyjson_is_num(value))
                    {
                        copiedValue = yyjson_mut_real(a_document, yyjson_get_num(value) * a_fadeMultiplier);
                    }
                    else
                    {
                        copiedValue = CopyFadeControllerValue(a_document, value, a_fadeMultiplier);
                    }
                    if (!copiedKey || !copiedValue || !yyjson_mut_obj_add(result, copiedKey, copiedValue))
                    {
                        return nullptr;
                    }
                }
                return result;
            }
            if (yyjson_is_arr(a_value))
            {
                auto* result = yyjson_mut_arr(a_document);
                std::size_t index = 0;
                std::size_t count = 0;
                yyjson_val* entry = nullptr;
                yyjson_arr_foreach(a_value, index, count, entry)
                {
                    auto* copy = CopyFadeControllerValue(a_document, entry, a_fadeMultiplier);
                    if (!copy || !yyjson_mut_arr_append(result, copy))
                    {
                        return nullptr;
                    }
                }
                return result;
            }
            return yyjson_val_mut_copy(a_document, a_value);
        }

        yyjson_mut_val* CopyColor(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value,
            const ColorTuning& a_tuning)
        {
            if (!yyjson_is_arr(a_value) || yyjson_arr_size(a_value) < 3)
            {
                return yyjson_val_mut_copy(a_document, a_value);
            }
            std::array<double, 3> channels{};
            for (std::size_t i = 0; i < channels.size(); ++i)
            {
                auto* channel = yyjson_arr_get(a_value, i);
                if (!yyjson_is_num(channel))
                {
                    return yyjson_val_mut_copy(a_document, a_value);
                }
                channels[i] = yyjson_get_num(channel);
            }
            const double sourceMaximum = std::ranges::max(channels);
            const double maximum = sourceMaximum <= 1.0001 ? 1.0 : std::max(255.0, sourceMaximum);
            const auto byte = [&](const double a_channel)
            {
                return static_cast<std::uint8_t>(std::clamp(std::round(a_channel / maximum * 255.0), 0.0, 255.0));
            };
            const RE::Color reference{ byte(channels[0]), byte(channels[1]), byte(channels[2]), 0 };
            const double luminance = 0.299 * channels[0] + 0.587 * channels[1] + 0.114 * channels[2];
            const double factor = std::max(0.0, a_tuning.saturationMultiplier) *
                                  WeatherPatcher::ColorHueScale(reference, a_tuning.hueScales, a_tuning.hueRanges);
            for (auto& channel : channels)
            {
                channel = std::clamp(luminance + (channel - luminance) * factor, 0.0, maximum);
            }
            const auto shift = WeatherPatcher::ColorHueShiftDegrees(reference, a_tuning.hueShift, a_tuning.hueRanges);
            if (std::abs(shift) > 0.0001)
            {
                const auto shifted = WeatherPatcher::RotateHuePreservingSaturationAndLuminance(
                    channels[0] / maximum,
                    channels[1] / maximum,
                    channels[2] / maximum,
                    shift);
                for (std::size_t i = 0; i < channels.size(); ++i)
                {
                    channels[i] = shifted[i] * maximum;
                }
            }
            auto* result = yyjson_mut_arr(a_document);
            for (const auto channel : channels)
            {
                yyjson_mut_arr_add_real(a_document, result, channel);
            }
            for (std::size_t i = 3; i < yyjson_arr_size(a_value); ++i)
            {
                yyjson_mut_arr_append(result, yyjson_val_mut_copy(a_document, yyjson_arr_get(a_value, i)));
            }
            return result;
        }

        yyjson_mut_val* CopyObject(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value,
            const double a_fadeMultiplier,
            const ColorTuning& a_tuning)
        {
            auto* result = yyjson_mut_obj(a_document);
            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_value);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                auto* value = yyjson_obj_iter_get_val(key);
                auto* copiedKey = yyjson_mut_strncpy(a_document, yyjson_get_str(key), yyjson_get_len(key));
                yyjson_mut_val* copiedValue = nullptr;
                if (KeyEquals(key, "fade") && yyjson_is_num(value))
                {
                    copiedValue = yyjson_mut_real(a_document, yyjson_get_num(value) * a_fadeMultiplier);
                }
                else if (KeyEquals(key, "fadeController"))
                {
                    copiedValue = CopyFadeControllerValue(a_document, value, a_fadeMultiplier);
                }
                else if (!kUseDirectLightPlacerNiLights && KeyEquals(key, "color"))
                {
                    copiedValue = CopyColor(a_document, value, a_tuning);
                }
                else
                {
                    copiedValue = CopyValue(a_document, value, a_fadeMultiplier, a_tuning);
                }
                if (!copiedKey || !copiedValue || !yyjson_mut_obj_add(result, copiedKey, copiedValue))
                {
                    return nullptr;
                }
            }
            return result;
        }

        yyjson_mut_val* CopyValue(
            yyjson_mut_doc* a_document,
            yyjson_val* a_value,
            const double a_fadeMultiplier,
            const ColorTuning& a_tuning)
        {
            if (yyjson_is_obj(a_value))
            {
                return CopyObject(a_document, a_value, a_fadeMultiplier, a_tuning);
            }
            if (yyjson_is_arr(a_value))
            {
                auto* result = yyjson_mut_arr(a_document);
                std::size_t index = 0;
                std::size_t count = 0;
                yyjson_val* entry = nullptr;
                yyjson_arr_foreach(a_value, index, count, entry)
                {
                    auto* copy = CopyValue(a_document, entry, a_fadeMultiplier, a_tuning);
                    if (!copy || !yyjson_mut_arr_append(result, copy))
                    {
                        return nullptr;
                    }
                }
                return result;
            }
            return yyjson_val_mut_copy(a_document, a_value);
        }

        yyjson_mut_val* CopyLightData(
            yyjson_mut_doc* a_document,
            yyjson_val* a_data,
            const Settings& a_settings,
            const WeatherPatcher::AmbientHueScaleValues& a_hueScales,
            const WeatherPatcher::HueRanges& a_hueRanges,
            const EmittanceMap& a_mappings)
        {
            if (!yyjson_is_obj(a_data))
            {
                return yyjson_val_mut_copy(a_document, a_data);
            }
            auto* lightValue = yyjson_obj_get(a_data, "light");
            const std::string lightEditorIDs = yyjson_is_str(lightValue) ?
                                                   std::string(yyjson_get_str(lightValue), yyjson_get_len(lightValue)) :
                                                   std::string{};
            const auto mappedEmittance = FindMappedEmittance(a_mappings, lightEditorIDs);
            if (yyjson_obj_get(a_data, "externalEmittance") || mappedEmittance)
            {
                auto* result = yyjson_val_mut_copy(a_document, a_data);
                if (!result || !mappedEmittance)
                {
                    return result;
                }
                yyjson_mut_obj_remove_key(result, "externalEmittance");
                return yyjson_mut_obj_add_strncpy(
                           a_document,
                           result,
                           "externalEmittance",
                           mappedEmittance->data(),
                           mappedEmittance->size()) ?
                           result :
                           nullptr;
            }
            const double fade = std::max(0.0, a_settings.fadeMultiplier);
            const double saturation = std::max(0.0, a_settings.saturationMultiplier);
            const ColorTuning tuning{ saturation, a_hueScales, a_settings.hueShift, a_hueRanges };
            return CopyObject(a_document, a_data, fade, tuning);
        }

        yyjson_mut_val* CopyLightEntry(
            yyjson_mut_doc* a_document,
            yyjson_val* a_entry,
            const Settings& a_settings,
            const WeatherPatcher::AmbientHueScaleValues& a_hueScales,
            const WeatherPatcher::HueRanges& a_hueRanges,
            const EmittanceMap& a_mappings)
        {
            if (!yyjson_is_obj(a_entry))
            {
                return yyjson_val_mut_copy(a_document, a_entry);
            }
            auto* result = yyjson_mut_obj(a_document);
            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_entry);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                auto* value = yyjson_obj_iter_get_val(key);
                auto* copiedKey = yyjson_mut_strncpy(a_document, yyjson_get_str(key), yyjson_get_len(key));
                auto* copiedValue = KeyEquals(key, "data") ?
                                        CopyLightData(a_document, value, a_settings, a_hueScales, a_hueRanges, a_mappings) :
                                        yyjson_val_mut_copy(a_document, value);
                if (!copiedKey || !copiedValue || !yyjson_mut_obj_add(result, copiedKey, copiedValue))
                {
                    return nullptr;
                }
            }
            return result;
        }

        yyjson_mut_val* CopyConfigEntry(
            yyjson_mut_doc* a_document,
            yyjson_val* a_entry,
            const Settings& a_settings,
            const WeatherPatcher::AmbientHueScaleValues& a_hueScales,
            const WeatherPatcher::HueRanges& a_hueRanges,
            const EmittanceMap& a_mappings)
        {
            if (!yyjson_is_obj(a_entry))
            {
                return yyjson_val_mut_copy(a_document, a_entry);
            }
            auto* result = yyjson_mut_obj(a_document);
            yyjson_obj_iter iterator = yyjson_obj_iter_with(a_entry);
            while (auto* key = yyjson_obj_iter_next(&iterator))
            {
                auto* value = yyjson_obj_iter_get_val(key);
                auto* copiedKey = yyjson_mut_strncpy(a_document, yyjson_get_str(key), yyjson_get_len(key));
                yyjson_mut_val* copiedValue = nullptr;
                if (KeyEquals(key, "lights") && yyjson_is_arr(value))
                {
                    copiedValue = yyjson_mut_arr(a_document);
                    std::size_t index = 0;
                    std::size_t count = 0;
                    yyjson_val* lightEntry = nullptr;
                    yyjson_arr_foreach(value, index, count, lightEntry)
                    {
                        auto* copy = CopyLightEntry(a_document, lightEntry, a_settings, a_hueScales, a_hueRanges, a_mappings);
                        if (!copy || !yyjson_mut_arr_append(copiedValue, copy))
                        {
                            return nullptr;
                        }
                    }
                }
                else
                {
                    copiedValue = yyjson_val_mut_copy(a_document, value);
                }
                if (!copiedKey || !copiedValue || !yyjson_mut_obj_add(result, copiedKey, copiedValue))
                {
                    return nullptr;
                }
            }
            return result;
        }

        std::optional<std::string> TransformLightPlacerJson(
            const std::string_view a_json,
            const Settings& a_settings,
            const WeatherPatcher::HueRanges& a_hueRanges,
            const EmittanceMap& a_mappings)
        {
            yyjson_read_err readError{};
            Document source(yyjson_read_opts(
                const_cast<char*>(a_json.data()), a_json.size(), YYJSON_READ_NOFLAG, nullptr, &readError));
            auto* root = source ? yyjson_doc_get_root(source.get()) : nullptr;
            if (!yyjson_is_arr(root))
            {
                return std::nullopt;
            }

            MutableDocument result(yyjson_mut_doc_new(nullptr));
            auto* resultRoot = yyjson_mut_arr(result.get());
            const auto hueScales = WeatherPatcher::ResolveHueScales(a_settings.hueScales);
            std::size_t index = 0;
            std::size_t count = 0;
            yyjson_val* entry = nullptr;
            yyjson_arr_foreach(root, index, count, entry)
            {
                auto* copy = CopyConfigEntry(result.get(), entry, a_settings, hueScales, a_hueRanges, a_mappings);
                if (!copy || !yyjson_mut_arr_append(resultRoot, copy))
                {
                    return std::nullopt;
                }
            }
            yyjson_mut_doc_set_root(result.get(), resultRoot);
            std::size_t length = 0;
            auto* data = yyjson_mut_write(result.get(), YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
            if (!data)
            {
                return std::nullopt;
            }
            std::string transformed(data, length);
            std::free(data);
            return transformed;
        }

        bool HasLightPlacerTuning(const Settings& a_settings)
        {
            const Settings defaults{};
            if constexpr (kUseDirectLightPlacerNiLights)
            {
                return a_settings.fadeMultiplier != defaults.fadeMultiplier;
            }
            return a_settings.fadeMultiplier != defaults.fadeMultiplier ||
                   a_settings.saturationMultiplier !=
                       defaults.saturationMultiplier ||
                   a_settings.hueScales != defaults.hueScales ||
                   a_settings.hueShift != defaults.hueShift;
        }

        bool TransformBrokeredLightPlacerJson(
            const char*,
            const char* a_input,
            const std::size_t a_inputSize,
            const XEMIAPI::LightPlacerOutput a_output,
            void* a_context)
        {
            try
            {
                if ((!a_input && a_inputSize != 0) || !a_output)
                {
                    return false;
                }
                std::optional<AppliedState> state;
                {
                    std::scoped_lock lock(brokerStateLock);
                    state = brokerTransformState;
                }
                if (!state ||
                    !HasLightPlacerTuning(state->settings))
                {
                    return a_output(
                        a_context,
                        a_input ? a_input : "",
                        a_inputSize);
                }
                static const EmittanceMap noLegacyMappings;
                const auto transformed =
                    TransformLightPlacerJson(
                        std::string_view(
                            a_input ? a_input : "",
                            a_inputSize),
                        state->settings,
                        state->hueRanges,
                        noLegacyMappings);
                return transformed &&
                       a_output(
                           a_context,
                           transformed->data(),
                           transformed->size());
            }
            catch (const std::exception& error)
            {
                logger::error(
                    "TuningUtil Light Placer transformer failed: {}",
                    error.what());
            }
            catch (...)
            {
                logger::error(
                    "TuningUtil Light Placer transformer failed with an unknown exception");
            }
            return false;
        }

        void OnBrokeredLightPlacerReloadComplete()
        {
            brokerReloadPending.store(false, std::memory_order_release);
            if (kUseDirectLightPlacerNiLights)
            {
                QueuePostReloadDirectLightPlacerRefresh();
            }
            if (!TuningSettings::IsTuningMenuEnabledForSession())
            {
                std::scoped_lock lock(brokerStateLock);
                brokerTransformState.reset();
            }
        }

        bool ConnectLightPlacerBroker()
        {
            if (brokerConnectionAttempted)
            {
                return xemiAPI != nullptr;
            }
            brokerConnectionAttempted = true;
            const auto module = GetModuleHandleW(L"XEMIUtil.dll");
            const auto request =
                module ?
                    reinterpret_cast<XEMIAPI::RequestInterface>(
                        GetProcAddress(
                            module,
                            "XEMIUtil_RequestAPI")) :
                    nullptr;
            xemiAPI =
                request ? request(XEMIAPI::kVersion) : nullptr;
            if (!xemiAPI ||
                xemiAPI->version != XEMIAPI::kVersion ||
                !xemiAPI->RegisterReferenceClient ||
                !xemiAPI->RegisterLightPlacerTransformer ||
                !xemiAPI->RequestLightPlacerReload)
            {
                xemiAPI = nullptr;
                DetailedLogging::Info(
                    "{}",
                    "XEMIUtil Light Placer broker is unavailable; using TuningUtil's standalone reload path");
                return false;
            }
            static const XEMIAPI::ReferenceCallbacks referenceCallbacks{
                .id = "TuningUtil",
                .OnReferenceEmittanceChanged = InitializeReference,
            };
            if (!xemiAPI->RegisterReferenceClient(
                    std::addressof(referenceCallbacks)))
            {
                xemiAPI = nullptr;
                logger::warn(
                    "XEMIUtil rejected TuningUtil's reference-emittance client; using the standalone Light Placer reload path");
                return false;
            }
            static const XEMIAPI::LightPlacerTransformer transformer{
                .id = "TuningUtil",
                .TransformJson = TransformBrokeredLightPlacerJson,
                .OnReloadComplete =
                    OnBrokeredLightPlacerReloadComplete,
            };
            if (!xemiAPI->RegisterLightPlacerTransformer(
                    std::addressof(transformer)))
            {
                xemiAPI = nullptr;
                logger::warn(
                    "XEMIUtil rejected TuningUtil's Light Placer transformer; using the standalone reload path");
                return false;
            }
            logger::info(
                "Connected TuningUtil point-light settings to XEMIUtil reference notifications and the Light Placer reload broker");
            return true;
        }

        bool ExecuteLightPlacerReload()
        {
            const char* command = nullptr;
            if (RE::SCRIPT_FUNCTION::LocateConsoleCommand("ReloadLP"))
            {
                command = "ReloadLP";
            }
            else if (RE::SCRIPT_FUNCTION::LocateConsoleCommand("lpreload"))
            {
                command = "lpreload";
            }
            if (!command)
            {
                DetailedLogging::Info("{}", "Light Placer reload command was not found; placed-light refresh skipped");
                return false;
            }
            auto* script = RE::IFormFactory::Create<RE::Script>();
            if (!script)
            {
                logger::error("Could not create the console script needed to reload Light Placer");
                return false;
            }
            script->SetCommand(command);
            script->CompileAndRun(nullptr);
            delete script;
            return true;
        }

        void ReloadLightPlacer(const Settings& a_settings, const WeatherPatcher::HueRanges& a_hueRanges)
        {
            if (!RE::SCRIPT_FUNCTION::LocateConsoleCommand("ReloadLP") &&
                !RE::SCRIPT_FUNCTION::LocateConsoleCommand("lpreload"))
            {
                return;
            }
            std::error_code error;
            if (!std::filesystem::is_directory(kLightPlacerRoot, error))
            {
                return;
            }

            struct Replacement
            {
                std::filesystem::path path;
                std::string original;
                std::string transformed;
            };
            std::vector<Replacement> replacements;
            const auto mappings = LoadEmittanceMappings();
            const auto options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator iterator(kLightPlacerRoot, options, error), end;
                iterator != end && !error;
                iterator.increment(error))
            {
                if (!iterator->is_regular_file(error) || Lowercase(iterator->path().extension().string()) != ".json")
                {
                    continue;
                }
                const auto original = ReadFile(iterator->path());
                if (!original)
                {
                    logger::warn("Could not read Light Placer config {}", iterator->path().string());
                    continue;
                }
                const auto transformed = TransformLightPlacerJson(*original, a_settings, a_hueRanges, mappings);
                if (!transformed)
                {
                    logger::warn("Could not transform Light Placer config {}; leaving it unchanged", iterator->path().string());
                    continue;
                }
                replacements.push_back({ iterator->path(), *original, *transformed });
            }

            std::size_t attempted = 0;
            bool writeSucceeded = true;
            for (; attempted < replacements.size(); ++attempted)
            {
                if (!WriteFile(replacements[attempted].path, replacements[attempted].transformed))
                {
                    logger::error("Could not temporarily update Light Placer config {}", replacements[attempted].path.string());
                    ++attempted;
                    writeSucceeded = false;
                    break;
                }
            }
            bool reloadSucceeded = false;
            if (writeSucceeded)
            {
                reloadSucceeded = ExecuteLightPlacerReload();
            }
            for (std::size_t i = 0; i < attempted; ++i)
            {
                if (!WriteFile(replacements[i].path, replacements[i].original))
                {
                    logger::critical("Could not restore Light Placer config {}", replacements[i].path.string());
                }
            }
            if (writeSucceeded)
            {
                logger::info("Point Lights refreshed {} Light Placer config(s) and restored their original files", attempted);
            }
            if (reloadSucceeded && kUseDirectLightPlacerNiLights)
            {
                QueuePostReloadDirectLightPlacerRefresh();
            }
        }

        void QueueLightPlacerReload(const Settings& a_settings, const WeatherPatcher::HueRanges& a_hueRanges)
        {
            const auto generation = ++reloadGeneration;
            auto task = [a_settings, a_hueRanges, generation]()
            {
                if (generation == reloadGeneration.load())
                {
                    ReloadLightPlacer(a_settings, a_hueRanges);
                }
            };
            if (auto* taskInterface = SKSE::GetTaskInterface())
            {
                taskInterface->AddTask(std::move(task));
            }
            else
            {
                task();
            }
        }

        bool RequiresLightPlacerReload(
            const std::optional<AppliedState>& a_previous,
            const Settings& a_settings,
            const WeatherPatcher::HueRanges& a_hueRanges)
        {
            const Settings defaults{};
            if constexpr (kUseDirectLightPlacerNiLights)
            {
                return a_previous ?
                           a_settings.fadeMultiplier != a_previous->settings.fadeMultiplier :
                           a_settings.fadeMultiplier != defaults.fadeMultiplier;
            }
            if (!a_previous)
            {
                return a_settings.fadeMultiplier != defaults.fadeMultiplier ||
                       a_settings.saturationMultiplier != defaults.saturationMultiplier ||
                       a_settings.hueScales != defaults.hueScales ||
                       a_settings.hueShift != defaults.hueShift;
            }

            const auto& previous = a_previous->settings;
            if (a_settings.fadeMultiplier != previous.fadeMultiplier ||
                a_settings.saturationMultiplier != previous.saturationMultiplier ||
                a_settings.hueScales != previous.hueScales ||
                a_settings.hueShift != previous.hueShift)
            {
                return true;
            }

            const bool hueTuningActive = a_settings.hueScales != defaults.hueScales ||
                                         a_settings.hueShift != defaults.hueShift ||
                                         previous.hueScales != defaults.hueScales ||
                                         previous.hueShift != defaults.hueShift;
            return hueTuningActive && a_hueRanges != a_previous->hueRanges;
        }

        void Saturate(RE::Color& a_color, const double a_multiplier)
        {
            const double luminance = 0.299 * a_color.red + 0.587 * a_color.green + 0.114 * a_color.blue;
            const double factor = std::max(0.0, a_multiplier);
            const auto channel = [&](const double a_value)
            {
                return static_cast<std::uint8_t>(std::clamp(std::round(luminance + (a_value - luminance) * factor), 0.0, 255.0));
            };
            a_color.red = channel(a_color.red);
            a_color.green = channel(a_color.green);
            a_color.blue = channel(a_color.blue);
        }

        bool NearlyEqual(const float a_left, const float a_right)
        {
            constexpr float absoluteTolerance = 0.00001f;
            constexpr float relativeTolerance = 0.0001f;
            return std::abs(a_left - a_right) <=
                   std::max(
                       absoluteTolerance,
                       relativeTolerance *
                           std::max(std::abs(a_left), std::abs(a_right)));
        }

        bool NearlyEqual(const RE::NiColor& a_left, const RE::NiColor& a_right)
        {
            return NearlyEqual(a_left.red, a_right.red) &&
                   NearlyEqual(a_left.green, a_right.green) &&
                   NearlyEqual(a_left.blue, a_right.blue);
        }

        RE::NiColor TuneLightPlacerDiffuse(
            const RE::NiColor& a_source,
            const AppliedState& a_state,
            const WeatherPatcher::AmbientHueScaleValues& a_hueScales)
        {
            std::array<double, 3> channels{
                a_source.red,
                a_source.green,
                a_source.blue,
            };
            const double maximum =
                std::max(1.0, std::ranges::max(channels));
            const auto byte = [&](const double a_channel)
            {
                return static_cast<std::uint8_t>(
                    std::clamp(
                        std::round(a_channel / maximum * 255.0),
                        0.0,
                        255.0));
            };
            const RE::Color reference{
                byte(channels[0]),
                byte(channels[1]),
                byte(channels[2]),
                0,
            };
            const double luminance =
                0.299 * channels[0] +
                0.587 * channels[1] +
                0.114 * channels[2];
            const double factor =
                std::max(
                    0.0,
                    a_state.settings.saturationMultiplier) *
                WeatherPatcher::ColorHueScale(
                    reference,
                    a_hueScales,
                    a_state.hueRanges);
            for (auto& channel : channels)
            {
                channel = std::clamp(
                    luminance + (channel - luminance) * factor,
                    0.0,
                    maximum);
            }
            const auto hueShift =
                WeatherPatcher::ColorHueShiftDegrees(
                    reference,
                    a_state.settings.hueShift,
                    a_state.hueRanges);
            if (std::abs(hueShift) > 0.0001)
            {
                const auto shifted =
                    WeatherPatcher::
                        RotateHuePreservingSaturationAndLuminance(
                            channels[0] / maximum,
                            channels[1] / maximum,
                            channels[2] / maximum,
                            hueShift);
                for (std::size_t index = 0;
                     index < channels.size();
                     ++index)
                {
                    channels[index] = shifted[index] * maximum;
                }
            }
            return RE::NiColor{
                static_cast<float>(channels[0]),
                static_cast<float>(channels[1]),
                static_cast<float>(channels[2]),
            };
        }

        bool IsLightPlacerNode(const RE::NiPointLight* a_light)
        {
            return a_light &&
                   static_cast<std::string_view>(a_light->name)
                       .starts_with(kLightPlacerNodePrefix);
        }

        bool TuneLightPlacerNode(
            RE::NiPointLight* a_light,
            const AppliedState& a_state,
            const WeatherPatcher::AmbientHueScaleValues& a_hueScales)
        {
            auto& runtime = a_light->GetLightRuntimeData();
            const auto name =
                std::string(static_cast<std::string_view>(a_light->name));
            auto baseline = lightPlacerRuntimeBaselines.find(a_light);
            if (baseline == lightPlacerRuntimeBaselines.end() ||
                baseline->second.name != name)
            {
                baseline =
                    lightPlacerRuntimeBaselines
                        .insert_or_assign(
                            a_light,
                            LightPlacerRuntimeBaseline{
                                .name = name,
                                .sourceDiffuse = runtime.diffuse,
                                .appliedDiffuse = {},
                                .hasApplied = false,
                            })
                        .first;
            }

            auto& captured = baseline->second;
            if (captured.hasApplied)
            {
                if (!NearlyEqual(
                        runtime.diffuse,
                        captured.appliedDiffuse))
                {
                    captured.sourceDiffuse = runtime.diffuse;
                }
            }

            const auto tunedDiffuse =
                TuneLightPlacerDiffuse(
                    captured.sourceDiffuse,
                    a_state,
                    a_hueScales);
            const bool changed =
                !NearlyEqual(runtime.diffuse, tunedDiffuse);
            runtime.diffuse = tunedDiffuse;
            captured.appliedDiffuse = tunedDiffuse;
            captured.hasApplied = true;
            return changed;
        }

        std::size_t RefreshDirectLightPlacerLights(
            const bool a_logEmptyResult = true)
        {
            if (!kUseDirectLightPlacerNiLights ||
                !directLightPlacerState)
            {
                return 0;
            }
            auto* tes = RE::TES::GetSingleton();
            if (!tes)
            {
                return 0;
            }

            const auto state = *directLightPlacerState;
            const auto hueScales =
                WeatherPatcher::ResolveHueScales(
                    state.settings.hueScales);
            std::unordered_set<RE::NiPointLight*> seen;
            std::size_t loadedReferenceCount = 0;
            std::size_t lightCount = 0;
            std::size_t changedCount = 0;
            tes->ForEachReference(
                [&](RE::TESObjectREFR* a_reference)
                {
                    auto* root =
                        a_reference ?
                            a_reference->GetCurrent3D() :
                            nullptr;
                    if (!root)
                    {
                        return RE::BSContainer::ForEachResult::
                            kContinue;
                    }
                    ++loadedReferenceCount;
                    RE::BSVisit::TraverseScenegraphLights(
                        root,
                        [&](RE::NiPointLight* a_light)
                        {
                            if (!IsLightPlacerNode(a_light) ||
                                !seen.insert(a_light).second)
                            {
                                return RE::BSVisit::
                                    BSVisitControl::kContinue;
                            }
                            ++lightCount;
                            changedCount +=
                                TuneLightPlacerNode(
                                    a_light,
                                    state,
                                    hueScales) ?
                                    1 :
                                    0;
                            return RE::BSVisit::
                                BSVisitControl::kContinue;
                        });
                    return RE::BSContainer::ForEachResult::
                        kContinue;
                });
            std::erase_if(
                lightPlacerRuntimeBaselines,
                [&](const auto& a_entry)
                {
                    return !seen.contains(a_entry.first);
                });
            if (a_logEmptyResult || lightCount != 0)
            {
                DetailedLogging::Info(
                    "Directly tuned {} of {} Light Placer NiLight node(s) across {} loaded reference(s)",
                    changedCount,
                    lightCount,
                    loadedReferenceCount);
            }
            return lightCount;
        }

        void SchedulePostReloadDirectLightPlacerRefresh(
            const std::uint64_t a_generation,
            const std::uint8_t a_attempt,
            const bool a_retryUntilFound)
        {
            std::thread(
                [a_generation, a_attempt, a_retryUntilFound]()
                {
                    std::this_thread::sleep_for(
                        kPostReloadRefreshDelay);
                    if (a_generation !=
                        directRefreshGeneration.load(
                            std::memory_order_acquire))
                    {
                        return;
                    }
                    auto task =
                        [a_generation,
                            a_attempt,
                            a_retryUntilFound]()
                    {
                        if (a_generation !=
                            directRefreshGeneration.load(
                                std::memory_order_acquire))
                        {
                            return;
                        }
                        const auto lightCount =
                            RefreshDirectLightPlacerLights(false);
                        if (lightCount != 0)
                        {
                            DetailedLogging::Info(
                                "Post-reload direct Light Placer refresh found {} NiLight node(s) on attempt {}/{}",
                                lightCount,
                                a_attempt,
                                kPostReloadRefreshAttempts);
                            return;
                        }
                        if (a_retryUntilFound &&
                            a_attempt <
                                kPostReloadRefreshAttempts)
                        {
                            SchedulePostReloadDirectLightPlacerRefresh(
                                a_generation,
                                static_cast<std::uint8_t>(
                                    a_attempt + 1),
                                true);
                            return;
                        }
                        if (a_retryUntilFound)
                        {
                            logger::warn(
                                "Post-reload direct Light Placer refresh found no NiLight nodes after {} bounded attempts; the next cell-load refresh remains active",
                                kPostReloadRefreshAttempts);
                        }
                        else
                        {
                            DetailedLogging::Info(
                                "{}",
                                "Post-reload direct Light Placer refresh found no previously loaded NiLight nodes; bounded retries were not needed");
                        }
                    };
                    if (auto* taskInterface =
                            SKSE::GetTaskInterface())
                    {
                        taskInterface->AddTask(
                            std::move(task));
                    }
                })
                .detach();
        }

        void QueuePostReloadDirectLightPlacerRefresh()
        {
            if (!kUseDirectLightPlacerNiLights)
            {
                return;
            }
            const bool retryUntilFound =
                !lightPlacerRuntimeBaselines.empty();
            const auto generation = ++directRefreshGeneration;
            SchedulePostReloadDirectLightPlacerRefresh(
                generation,
                1,
                retryUntilFound);
        }

        void QueueDirectLightPlacerRefresh()
        {
            if (!kUseDirectLightPlacerNiLights)
            {
                return;
            }
            const auto generation = ++directRefreshGeneration;
            auto task = [generation]()
            {
                if (generation ==
                    directRefreshGeneration.load(
                        std::memory_order_acquire))
                {
                    RefreshDirectLightPlacerLights();
                }
            };
            if (auto* taskInterface = SKSE::GetTaskInterface())
            {
                taskInterface->AddTask(std::move(task));
            }
            else
            {
                task();
            }
        }

        class CellFullyLoadedEventSink final :
            public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellFullyLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*)
                override
            {
                if (a_event && a_event->cell)
                {
                    QueueDirectLightPlacerRefresh();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        CellFullyLoadedEventSink cellFullyLoadedEventSink;

        bool ApplyReferenceFadeOverride(RE::TESObjectREFR* a_reference, const double a_multiplier)
        {
            auto* light = GetBaseLight(a_reference);
            auto* extraLightData = a_reference ? a_reference->extraList.GetByType<RE::ExtraLightData>() : nullptr;
            if (!light || !extraLightData || extraLightData->data.fade <= 0.0f)
            {
                return false;
            }

            const auto baseline = referenceFadeBaselines.try_emplace(a_reference, extraLightData->data.fade).first->second;
            if (HasExternalEmittance(a_reference))
            {
                extraLightData->data.fade = baseline;
                return false;
            }
            extraLightData->data.fade = baseline * static_cast<float>(std::max(0.0, a_multiplier));
            return true;
        }

        void ApplyReferenceFadeOverrides(RE::TESDataHandler* a_dataHandler, const double a_multiplier)
        {
            std::size_t adjusted = 0;
            for (auto* reference : a_dataHandler->GetFormArray<RE::TESObjectREFR>())
            {
                adjusted += ApplyReferenceFadeOverride(reference, a_multiplier) ? 1 : 0;
            }
            DetailedLogging::Info("Adjusted {} reference-level point-light fade override(s)", adjusted);
        }

        void LogPointLightColorDiagnostic(
            RE::TESObjectREFR* a_reference,
            RE::TESObjectLIGH* a_light,
            const std::string_view a_phase)
        {
            if (!DetailedLogging::IsEnabled() || !a_reference || !a_light ||
                a_reference->GetFormID() != kPointLightColorDiagnosticReference)
            {
                return;
            }

            const auto baseline = baselines.find(a_light);
            const auto* extraLight =
                a_reference->extraList.GetByType<RE::ExtraLight>();
            const auto* runtimeLight =
                extraLight && extraLight->lightData ?
                    extraLight->lightData->light.get() :
                    nullptr;
            if (!runtimeLight)
            {
                DetailedLogging::Info(
                    "Point-light color diagnostic {} for reference {:08X}, base {:08X}: captured base RGB=({}, {}, {}), tuned base RGB=({}, {}, {}), loaded runtime light unavailable",
                    a_phase,
                    a_reference->GetFormID(),
                    a_light->GetFormID(),
                    baseline != baselines.end() ?
                        static_cast<std::uint32_t>(baseline->second.color.red) :
                        0,
                    baseline != baselines.end() ?
                        static_cast<std::uint32_t>(baseline->second.color.green) :
                        0,
                    baseline != baselines.end() ?
                        static_cast<std::uint32_t>(baseline->second.color.blue) :
                        0,
                    static_cast<std::uint32_t>(a_light->data.color.red),
                    static_cast<std::uint32_t>(a_light->data.color.green),
                    static_cast<std::uint32_t>(a_light->data.color.blue));
                return;
            }

            const auto& runtime = runtimeLight->GetLightRuntimeData();
            DetailedLogging::Info(
                "Point-light color diagnostic {} for reference {:08X}, base {:08X}: captured base RGB=({}, {}, {}), tuned base RGB=({}, {}, {}), runtime diffuse=({:.6f}, {:.6f}, {:.6f}), ambient=({:.6f}, {:.6f}, {:.6f}), fade={:.6f}",
                a_phase,
                a_reference->GetFormID(),
                a_light->GetFormID(),
                baseline != baselines.end() ?
                    static_cast<std::uint32_t>(baseline->second.color.red) :
                    0,
                baseline != baselines.end() ?
                    static_cast<std::uint32_t>(baseline->second.color.green) :
                    0,
                baseline != baselines.end() ?
                    static_cast<std::uint32_t>(baseline->second.color.blue) :
                    0,
                static_cast<std::uint32_t>(a_light->data.color.red),
                static_cast<std::uint32_t>(a_light->data.color.green),
                static_cast<std::uint32_t>(a_light->data.color.blue),
                runtime.diffuse.red,
                runtime.diffuse.green,
                runtime.diffuse.blue,
                runtime.ambient.red,
                runtime.ambient.green,
                runtime.ambient.blue,
                runtime.fade);
        }

        bool RefreshLoadedLightReference(RE::TESObjectREFR* a_reference)
        {
            auto* light = GetBaseLight(a_reference);
            if (!light || !a_reference->Is3DLoaded())
            {
                return false;
            }

            LogPointLightColorDiagnostic(a_reference, light, "before UpdateRefLight");
            a_reference->UpdateRefLight();
            LogPointLightColorDiagnostic(a_reference, light, "after UpdateRefLight");
            if (auto* extraLight = a_reference->extraList.GetByType<RE::ExtraLight>();
                extraLight && extraLight->lightData && extraLight->lightData->light)
            {
                const auto* extraLightData =
                    a_reference->extraList.GetByType<RE::ExtraLightData>();
                auto& runtime = extraLight->lightData->light->GetLightRuntimeData();
                constexpr float colorChannelScale = 1.0f / 255.0f;
                runtime.diffuse.red =
                    static_cast<float>(light->data.color.red) * colorChannelScale;
                runtime.diffuse.green =
                    static_cast<float>(light->data.color.green) * colorChannelScale;
                runtime.diffuse.blue =
                    static_cast<float>(light->data.color.blue) * colorChannelScale;
                runtime.fade =
                    extraLightData && extraLightData->data.fade > 0.0f ?
                        extraLightData->data.fade :
                        light->fade;
                LogPointLightColorDiagnostic(
                    a_reference,
                    light,
                    "after runtime synchronization");
            }
            return true;
        }

        void RefreshLoadedLightReferences()
        {
            auto* tes = RE::TES::GetSingleton();
            if (!tes)
            {
                return;
            }
            std::size_t refreshed = 0;
            tes->ForEachReference([&](RE::TESObjectREFR* a_reference)
                {
                if (GetBaseLight(a_reference))
                {
                    if (referenceFadeMultiplier)
                    {
                        ApplyReferenceFadeOverride(a_reference, *referenceFadeMultiplier);
                    }
                    refreshed += RefreshLoadedLightReference(a_reference) ? 1 : 0;
                }
                return RE::BSContainer::ForEachResult::kContinue; });
            DetailedLogging::Info("Refreshed {} loaded point-light reference(s)", refreshed);
        }

    }  // namespace

    void Apply(
        const Settings& a_settings,
        const WeatherPatcher::HueRanges& a_hueRanges,
        const bool a_commitLightPlacer)
    {
        const AppliedState state{ a_settings, a_hueRanges };
        const bool recordsChanged = !appliedState || *appliedState != state;
        if (kUseDirectLightPlacerNiLights)
        {
            directLightPlacerState = state;
        }
        const bool brokerAvailable = ConnectLightPlacerBroker();
        if (brokerAvailable)
        {
            std::scoped_lock lock(brokerStateLock);
            brokerTransformState = state;
        }
        if (!recordsChanged && !a_commitLightPlacer)
        {
            return;
        }
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            return;
        }

        if (recordsChanged)
        {
            std::size_t lightCount = 0;
            std::size_t externalEmittanceCount = 0;
            const auto hueScales = WeatherPatcher::ResolveHueScales(a_settings.hueScales);
            for (auto* light : dataHandler->GetFormArray<RE::TESObjectLIGH>())
            {
                if (!light)
                {
                    continue;
                }
                const auto baseline = baselines.try_emplace(light, Baseline{ light->fade, light->data.color }).first;
                light->fade = baseline->second.fade;
                light->data.color = baseline->second.color;
                if (externallyEmissiveLights.contains(light))
                {
                    ++externalEmittanceCount;
                    continue;
                }

                const double fade = std::max(0.0, a_settings.fadeMultiplier);
                const double saturation = std::max(0.0, a_settings.saturationMultiplier) *
                                          WeatherPatcher::ColorHueScale(baseline->second.color, hueScales, a_hueRanges);
                const auto hueShift = WeatherPatcher::ColorHueShiftDegrees(
                    baseline->second.color,
                    a_settings.hueShift,
                    a_hueRanges);
                light->fade *= static_cast<float>(fade);
                Saturate(light->data.color, saturation);
                WeatherPatcher::ShiftHue(light->data.color, hueShift);
                ++lightCount;
            }

            appliedState = state;
            ApplyReferenceFadeOverrides(dataHandler, a_settings.fadeMultiplier);
            if (a_settings.fadeMultiplier == 1.0)
            {
                referenceFadeMultiplier.reset();
            }
            else
            {
                referenceFadeMultiplier = a_settings.fadeMultiplier;
            }
            RefreshLoadedLightReferences();
            RefreshDirectLightPlacerLights();
            logger::info(
                "Point Lights applied to {} light record(s); excluded {} XEMI-backed light record(s)",
                lightCount,
                externalEmittanceCount);
        }

        if (a_commitLightPlacer)
        {
            if (RequiresLightPlacerReload(lightPlacerState, a_settings, a_hueRanges))
            {
                bool brokered = false;
                if (brokerAvailable)
                {
                    brokerReloadPending.store(
                        true,
                        std::memory_order_release);
                    brokered = xemiAPI->RequestLightPlacerReload();
                    if (!brokered)
                    {
                        brokerReloadPending.store(
                            false,
                            std::memory_order_release);
                    }
                }
                if (brokered)
                {
                    logger::info(
                        "Point Lights requested a combined XEMIUtil and TuningUtil Light Placer reload");
                }
                else
                {
                    QueueLightPlacerReload(a_settings, a_hueRanges);
                }
            }
            lightPlacerState = state;
        }
    }

    void InstallRuntimeEvents()
    {
        if (runtimeEventsInstalled)
        {
            return;
        }
        if (!kUseDirectLightPlacerNiLights)
        {
            logger::info(
                "Point Lights is using the retained Light Placer JSON transform and reload path");
            runtimeEventsInstalled = true;
            return;
        }
        auto* eventSource =
            RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSource)
        {
            logger::warn(
                "Point Lights could not register its direct Light Placer cell-load refresh");
            return;
        }
        eventSource->AddEventSink(
            std::addressof(cellFullyLoadedEventSink));
        runtimeEventsInstalled = true;
        logger::info(
            "Point Lights enabled direct Light Placer NiLight color tuning and JSON fade reloads");
    }

    void ReleaseRuntimeState()
    {
        appliedState.reset();
        lightPlacerState.reset();
        if (!brokerReloadPending.load(std::memory_order_acquire))
        {
            std::scoped_lock lock(brokerStateLock);
            brokerTransformState.reset();
        }
    }

    void InitializeReference(RE::TESObjectREFR* a_reference)
    {
        if (HasExternalEmittance(a_reference))
        {
            if (auto* light = GetBaseLight(a_reference))
            {
                externallyEmissiveLights.insert(light);
                bool baseRestored = false;
                if (const auto baseline = baselines.find(light); baseline != baselines.end())
                {
                    light->fade = baseline->second.fade;
                    light->data.color = baseline->second.color;
                    baseRestored = true;
                }
                const auto* extraLightData =
                    a_reference->extraList.GetByType<RE::ExtraLightData>();
                const bool referenceFadeRestored =
                    extraLightData && extraLightData->data.fade > 0.0f;
                ApplyReferenceFadeOverride(a_reference, 1.0);
                const bool loadedLightRefreshed =
                    RefreshLoadedLightReference(a_reference);
                DetailedLogging::Info(
                    "Registered externally emissive light reference {:08X} with base {:08X}; base restored={}, reference fade restored={}, loaded light refreshed={}",
                    a_reference->GetFormID(),
                    light->GetFormID(),
                    baseRestored,
                    referenceFadeRestored,
                    loadedLightRefreshed);
            }
            return;
        }
        if (referenceFadeMultiplier)
        {
            ApplyReferenceFadeOverride(a_reference, *referenceFadeMultiplier);
        }
    }
}  // namespace MPL::PointLightPatcher
