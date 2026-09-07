#include <SliderStorage.h>
#include <SliderIdentity.h>
#include <SliderSettingCatalog.h>
#include <JsonOverlay.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <yyjson.h>

namespace MPL::SliderStorage
{
    namespace
    {
        struct ReadDeleter { void operator()(yyjson_doc* a_value) const { yyjson_doc_free(a_value); } };
        struct WriteDeleter { void operator()(yyjson_mut_doc* a_value) const { yyjson_mut_doc_free(a_value); } };
        using Document = std::unique_ptr<yyjson_doc, ReadDeleter>;
        using Mutable = std::unique_ptr<yyjson_mut_doc, WriteDeleter>;
        constexpr std::array runtimeRuleMaps{
            "filteredWeatherAdjustments", "filteredLightingTemplateAdjustments",
            "filteredBaseLightAdjustments", "filteredObjectLightingAdjustments" };
        constexpr std::array moduleCompressionPaths{
            std::string_view{ "withinWeatherCompression.ambient" },
            std::string_view{ "betweenWeatherCompression.ambient" },
            std::string_view{ "withinWeatherCompression.sunlight" },
            std::string_view{ "betweenWeatherCompression.sunlight" },
        };

        Document Parse(const std::string_view a_text)
        {
            return Document(yyjson_read(a_text.data(), a_text.size(), 0));
        }
        std::string Text(yyjson_val* a_object, const char* a_key)
        {
            auto* value = yyjson_is_obj(a_object) ? yyjson_obj_get(a_object, a_key) : nullptr;
            return yyjson_is_str(value) ? yyjson_get_str(value) : "";
        }
        yyjson_val* Member(yyjson_val* a_object, const std::string_view a_key)
        {
            return yyjson_is_obj(a_object) ? yyjson_obj_getn(a_object, a_key.data(), a_key.size()) : nullptr;
        }
        yyjson_val* Find(yyjson_val* a_object, const std::string_view a_path)
        {
            const auto separator = a_path.find('.');
            if (separator == a_path.npos) return Member(a_object, a_path);
            const auto first = a_path.substr(0, separator);
            if (first == "sliderValues" || std::ranges::find(runtimeRuleMaps, first) != runtimeRuleMaps.end())
                return Member(Member(a_object, first), a_path.substr(separator + 1));
            return Find(Member(a_object, first), a_path.substr(separator + 1));
        }
        std::optional<double> Numeric(yyjson_val* a_value)
        {
            if (!yyjson_is_num(a_value)) return std::nullopt;
            const auto result = yyjson_get_num(a_value);
            return std::isfinite(result) ? std::optional{ result } : std::nullopt;
        }
        void Put(yyjson_mut_doc* a_doc, yyjson_mut_val* a_object, const std::string_view a_key, const double a_value)
        {
            yyjson_mut_obj_put(a_object, yyjson_mut_strncpy(a_doc, a_key.data(), a_key.size()), yyjson_mut_real(a_doc, a_value));
        }
        yyjson_mut_val* Object(yyjson_mut_doc* a_doc, yyjson_mut_val* a_parent, const std::string_view a_key)
        {
            auto* object = yyjson_mut_obj_getn(a_parent, a_key.data(), a_key.size());
            if (!yyjson_mut_is_obj(object))
            {
                object = yyjson_mut_obj(a_doc);
                yyjson_mut_obj_put(a_parent, yyjson_mut_strncpy(a_doc, a_key.data(), a_key.size()), object);
            }
            return object;
        }
        void PutPath(yyjson_mut_doc* a_doc, yyjson_mut_val* a_object, const std::string_view a_path, const double a_value)
        {
            const auto separator = a_path.find('.');
            if (separator == a_path.npos) Put(a_doc, a_object, a_path, a_value);
            else PutPath(a_doc, Object(a_doc, a_object, a_path.substr(0, separator)), a_path.substr(separator + 1), a_value);
        }
        void Remove(yyjson_mut_val* a_object, const std::string_view a_path)
        {
            if (!yyjson_mut_is_obj(a_object)) return;
            const auto separator = a_path.find('.');
            if (separator == a_path.npos) yyjson_mut_obj_remove_keyn(a_object, a_path.data(), a_path.size());
            else
            {
                auto* child = yyjson_mut_obj_getn(a_object, a_path.data(), separator);
                Remove(child, a_path.substr(separator + 1));
                if (yyjson_mut_is_obj(child) && yyjson_mut_obj_size(child) == 0)
                    yyjson_mut_obj_remove_keyn(a_object, a_path.data(), separator);
            }
        }
        std::optional<std::string> Write(yyjson_mut_doc* a_doc, std::string& a_error)
        {
            std::size_t length = 0;
            auto* text = yyjson_mut_write(a_doc, YYJSON_WRITE_PRETTY, &length);
            if (!text) { a_error = "Slider values could not be serialized."; return std::nullopt; }
            std::string result(text, length);
            std::free(text);
            return result;
        }
        std::vector<Target> Targets(yyjson_val* a_module)
        {
            std::vector<Target> result;
            const auto add = [&](yyjson_val* a_value)
            {
                auto path = yyjson_is_str(a_value) ? std::string(yyjson_get_str(a_value)) : Text(a_value, "setting");
                if (SliderSettingCatalog::Find(path)) result.push_back({ std::move(path), Numeric(Member(a_value, "scale")).value_or(1.0) });
            };
            auto* settings = Member(a_module, "settings");
            if (yyjson_is_arr(settings))
            {
                std::size_t index, count; yyjson_val* value;
                yyjson_arr_foreach(settings, index, count, value) add(value);
            }
            if (result.empty()) add(Member(a_module, "setting"));
            return result;
        }
        std::string FilterMap(yyjson_val* a_module)
        {
            if (yyjson_is_obj(Member(a_module, "baseObjectFilter"))) return runtimeRuleMaps[3];
            if (yyjson_is_obj(Member(a_module, "baseLightFilter"))) return runtimeRuleMaps[2];
            if (yyjson_is_obj(Member(a_module, "lightingTemplateFilter"))) return runtimeRuleMaps[1];
            if (yyjson_is_obj(Member(a_module, "weatherFilter")) || yyjson_is_arr(Member(a_module, "times")) ||
                yyjson_is_obj(Member(a_module, "hueScales")) || yyjson_is_bool(Member(a_module, "ignoreProfileFilters")))
                return runtimeRuleMaps[0];
            return {};
        }
    }

    std::vector<ModuleSlider> ModuleSliders(const std::string_view a_category)
    {
        std::vector<ModuleSlider> result;
        std::string prefix;
        if (a_category == "brightness") prefix = "brightnessMultiplier.";
        else if (a_category == "saturation") prefix = "saturationMultiplier.";
        else if (a_category == "hueShift") prefix = "hueShift.";
        else if (a_category == "lightBrightness") prefix = "lightBrightnessMultiplier.";
        else if (a_category == "lightSaturation") prefix = "lightSaturationMultiplier.";
        else if (a_category == "lightHueShift") prefix = "lightHueShift.";
        else if (a_category == "pointLights.hueShift") prefix = "pointLights.hueShift.";
        else if (a_category == "exteriorImageSpace" || a_category == "lightImageSpace") prefix = std::string(a_category) + ".";
        if (!prefix.empty())
            for (const auto& entry : SliderSettingCatalog::Entries())
                if (entry.path.starts_with(prefix))
                    result.emplace_back(entry.path, entry.label);
        if (a_category == "brightness") result.emplace_back("volumetricLightingIntensityMultiplier", "Volumetric Lighting Intensity");
        if (a_category == "lightBrightness")
        {
            result.emplace_back("lightFogPowerMultiplier", "Fog Power");
            result.emplace_back("lightFogMaxMultiplier", "Fog Strength");
        }
        if (a_category == "pointLights")
        {
            result.emplace_back("pointLights.fadeMultiplier", "Brightness");
            result.emplace_back("pointLights.radiusMultiplier", "Radius");
            result.emplace_back("pointLights.saturationMultiplier", "Saturation");
        }
        if (a_category == "ambientCompression" || a_category == "sunlightCompression")
        {
            const std::string field = a_category == "ambientCompression" ? "ambient" : "sunlight";
            result.emplace_back("brightnessMultiplier." + field, "Brightness");
            result.emplace_back("withinWeatherCompression." + field, "Night Brightness");
            result.emplace_back("betweenWeatherCompression." + field, "Dark Weather Brightness");
        }
        for (auto& slider : result)
        {
            const auto& path = slider.setting;
            if (a_category == "brightness" && path != "volumetricLightingIntensityMultiplier") slider.minimum = 0.1f;
            if (a_category == "saturation" || a_category == "lightSaturation") slider.maximum = 6.0f;
            if (a_category == "lightBrightness" && path.starts_with("lightBrightnessMultiplier.")) slider.maximum = 10.0f;
            if (a_category == "pointLights") slider.maximum = path == "pointLights.saturationMultiplier" ? 6.0f : 10.0f;
            if (a_category == "hueShift" || a_category == "lightHueShift" || a_category == "pointLights.hueShift")
            {
                slider.minimum = -180.0f;
                slider.maximum = 180.0f;
            }
            if (a_category == "ambientCompression" || a_category == "sunlightCompression")
            {
                if (path.starts_with("brightnessMultiplier."))
                {
                    slider.minimum = 0.1f;
                    slider.maximum = 2.0f;
                }
                else
                {
                    slider.minimum = -200.0f;
                    slider.maximum = 100.0f;
                    slider.step = 10.0f;
                    slider.format = "%.0f%%";
                }
            }
        }
        return result;
    }

    const std::vector<std::string>& AdjustmentPaths()
    {
        static const auto paths = []
        {
            std::vector<std::string> result;
            for (const auto& entry : SliderSettingCatalog::Entries()) result.push_back(entry.path);
            for (const auto path : moduleCompressionPaths) result.emplace_back(path);
            return result;
        }();
        return paths;
    }

    bool IsAdjustment(const std::string_view a_path)
    {
        return SliderSettingCatalog::Find(a_path) != nullptr || std::ranges::contains(moduleCompressionPaths, a_path);
    }
    double Neutral(const std::string_view a_path)
    {
        return a_path.starts_with("hueShift.") || a_path.starts_with("lightHueShift.") ||
            a_path.starts_with("pointLights.hueShift.") || std::ranges::contains(moduleCompressionPaths, a_path) ? 0.0 : 1.0;
    }
    double Combine(const std::string_view a_path, const double a_left, const double a_right)
    {
        const auto neutral = Neutral(a_path);
        if (a_left == neutral) return a_right;
        if (a_right == neutral) return a_left;
        if (std::ranges::contains(moduleCompressionPaths, a_path))
            return 100.0 * (1.0 - (1.0 - a_left / 100.0) * (1.0 - a_right / 100.0));
        return Neutral(a_path) == 0.0 ? a_left + a_right : a_left * a_right;
    }
    double Scaled(const std::string_view a_path, const double a_value, const double a_scale)
    {
        if (a_scale == 1.0) return a_value;
        const auto neutral = Neutral(a_path);
        return neutral + (a_value - neutral) * a_scale;
    }

    std::vector<Binding> ReadLayout(const std::string_view a_json, std::string& a_error)
    {
        const auto document = Parse(a_json);
        return ReadLayout(document ? yyjson_doc_get_root(document.get()) : nullptr, a_error);
    }

    std::vector<Binding> ReadLayout(yyjson_val* a_root, std::string& a_error)
    {
        a_error.clear();
        auto* pages = Member(a_root, "pages");
        if (!yyjson_is_arr(pages)) { a_error = "The slider layout has no pages."; return {}; }
        std::vector<Binding> bindings;
        std::size_t p, pageCount; yyjson_val* page;
        yyjson_arr_foreach(pages, p, pageCount, page)
        {
            std::vector<std::string> boxes;
            auto* modules = Member(page, "modules");
            if (!yyjson_is_arr(modules)) continue;
            std::size_t m, moduleCount; yyjson_val* module;
            yyjson_arr_foreach(modules, m, moduleCount, module)
            {
                const auto type = Text(module, "type");
                if (type == "dropBoxStart")
                {
                    auto label = Text(module, "label");
                    if (label.empty()) label = Text(module, "header");
                    boxes.push_back(std::move(label));
                    continue;
                }
                if (type == "dropBoxEnd") { if (!boxes.empty()) boxes.pop_back(); continue; }
                if (type != "slider" && type != "settings") continue;
                auto controlID = Text(module, "id");
                if (controlID.empty()) controlID = std::format("~control{}-{}", p, m);
                const auto filter = FilterMap(module);
                const auto add = [&](std::vector<Target> a_targets, std::string a_label, std::string a_suffix)
                {
                    if (a_targets.empty()) return;
                    auto hierarchy = boxes;
                    if (type == "settings")
                        if (const auto separator = a_label.find(" / "); separator != a_label.npos)
                        {
                            hierarchy.push_back(a_label.substr(0, separator));
                            a_label.erase(0, separator + 3);
                        }
                    const auto id = SliderIdentity::Make(Text(page, "title"), hierarchy, a_label);
                    const auto ruleID = controlID + a_suffix;
                    bindings.push_back({ p, m, id, id, controlID, ruleID, filter,
                        std::move(a_targets), type == "settings" });
                    bindings.back().neutral = Neutral(bindings.back().targets.front().path);
                };
                if (type == "slider")
                {
                    auto targets = Targets(module);
                    auto label = Text(module, "label");
                    if (label.empty()) label = controlID;
                    if (label.empty() && !targets.empty()) label = targets.front().path;
                    add(std::move(targets), std::move(label), {});
                }
                else
                {
                    for (const auto& slider : ModuleSliders(Text(module, "setting")))
                    {
                        const auto& path = slider.setting;
                        if (!filter.empty() && path == "volumetricLightingIntensityMultiplier") continue;
                        const auto* entry = SliderSettingCatalog::Find(path);
                        auto suffix = entry && !entry->target.empty() ? "_" + SliderIdentity::ComparisonKey(entry->target) : "_" + path;
                        if (entry && !entry->hue.empty()) suffix += "_" + entry->hue;
                        add({ { path, 1.0 } }, slider.label, std::move(suffix));
                    }
                }
            }
        }
        std::unordered_map<std::string, std::size_t> counts;
        for (const auto& binding : bindings) ++counts[SliderIdentity::ComparisonKey(binding.id)];
        for (auto& binding : bindings)
            if (counts[SliderIdentity::ComparisonKey(binding.id)] > 1)
                binding.valueID += std::format("~draft{}-{}", binding.page, binding.module);
        return bindings;
    }

    std::optional<std::string> Store(const std::string_view a_json, const std::span<const Binding> a_bindings,
        const bool a_defaults, std::string& a_error)
    {
        a_error.clear();
        const auto source = Parse(a_json);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot)) { a_error = "Slider settings are not a JSON object."; return std::nullopt; }
        Mutable document(yyjson_doc_mut_copy(source.get(), nullptr));
        auto* root = yyjson_mut_doc_get_root(document.get());
        auto* values = Object(document.get(), root, "sliderValues");
        auto* existing = Member(sourceRoot, "sliderValues");
        for (const auto& binding : a_bindings)
        {
            if (binding.controlID == "$sliderCreatorPreview")
            {
                yyjson_mut_obj_remove_keyn(values, binding.valueID.data(), binding.valueID.size());
                for (const auto& target : binding.targets) Remove(root, target.path);
                continue;
            }
            auto value = Numeric(Member(existing, binding.valueID));
            if (binding.ruleValueMap.empty())
            {
                for (const auto& target : binding.targets)
                    Remove(root, target.path);
            }
            if (value || a_defaults) Put(document.get(), values, binding.valueID, value.value_or(binding.neutral));
        }
        for (const auto map : runtimeRuleMaps) yyjson_mut_obj_remove_key(root, map);
        if (yyjson_mut_obj_size(values) == 0) yyjson_mut_obj_remove_key(root, "sliderValues");
        return Write(document.get(), a_error);
    }

    std::optional<std::string> Materialize(const std::string_view a_json, const std::span<const Binding> a_bindings, std::string& a_error)
    {
        const auto source = Parse(a_json);
        auto* sourceRoot = source ? yyjson_doc_get_root(source.get()) : nullptr;
        if (!yyjson_is_obj(sourceRoot)) { a_error = "Slider settings are not a JSON object."; return std::nullopt; }
        Mutable document(yyjson_doc_mut_copy(source.get(), nullptr));
        auto* root = yyjson_mut_doc_get_root(document.get());
        auto* values = Member(sourceRoot, "sliderValues");
        for (const auto map : runtimeRuleMaps) yyjson_mut_obj_remove_key(root, map);
        std::map<std::string, double> targets;
        for (const auto& binding : a_bindings)
        {
            const auto value = Numeric(Member(values, binding.valueID)).value_or(binding.neutral);
            if (!binding.ruleValueMap.empty())
                Put(document.get(), Object(document.get(), root, binding.ruleValueMap), binding.ruleID, value);
            else for (const auto& target : binding.targets)
            {
                auto [position, inserted] = targets.try_emplace(target.path, Neutral(target.path));
                position->second = Combine(target.path, position->second, Scaled(target.path, value, target.scale));
            }
        }
        for (const auto& [path, value] : targets) PutPath(document.get(), root, path, value);
        return Write(document.get(), a_error);
    }

    std::optional<std::string> Stack(const std::span<const std::string> a_profiles, std::string& a_error)
    {
        std::string combined = "{}";
        std::map<std::string, double> values;
        for (const auto& profile : a_profiles)
        {
            auto next = JsonOverlay::Overlay(combined, profile, a_error);
            if (!next) return std::nullopt;
            combined = std::move(*next);
            const auto document = Parse(profile);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            for (const auto& path : AdjustmentPaths())
            {
                if (const auto value = Numeric(Find(root, path)))
                {
                    auto [position, inserted] = values.try_emplace(path, Neutral(path));
                    position->second = Combine(path, position->second, *value);
                }
            }
        }
        const auto source = Parse(combined);
        Mutable document(source ? yyjson_doc_mut_copy(source.get(), nullptr) : nullptr);
        if (!document) return std::nullopt;
        auto* root = yyjson_mut_doc_get_root(document.get());
        for (const auto& [path, value] : values) PutPath(document.get(), root, path, value);
        return Write(document.get(), a_error);
    }

    std::optional<std::string> Remap(const std::string_view a_json, const std::span<const Binding> a_old,
        const std::span<const Binding> a_new, std::string& a_error)
    {
        const auto stored = Store(a_json, a_old, false, a_error);
        const auto source = stored ? Parse(*stored) : nullptr;
        if (!source) return std::nullopt;
        auto* oldValues = Member(yyjson_doc_get_root(source.get()), "sliderValues");
        Mutable document(yyjson_doc_mut_copy(source.get(), nullptr));
        auto* root = yyjson_mut_doc_get_root(document.get());
        auto* values = Object(document.get(), root, "sliderValues");
        std::vector<std::pair<std::string, double>> moved;
        for (const auto& previous : a_old)
        {
            auto next = std::ranges::find_if(a_new, [&](const auto& candidate)
            {
                return !previous.controlID.empty() && previous.controlID == candidate.controlID &&
                    (!previous.moduleSlider || previous.targets == candidate.targets);
            });
            if (next == a_new.end()) next = std::ranges::find_if(a_new, [&](const auto& candidate)
                { return previous.valueID == candidate.valueID; });
            if (next != a_new.end() && next->valueID == previous.valueID) continue;
            if (const auto value = Numeric(Member(oldValues, previous.valueID)))
            {
                yyjson_mut_obj_remove_keyn(values, previous.valueID.data(), previous.valueID.size());
                if (next != a_new.end()) moved.emplace_back(next->valueID, *value);
            }
        }
        for (const auto& [id, value] : moved) Put(document.get(), values, id, value);
        if (yyjson_mut_obj_size(values) == 0) yyjson_mut_obj_remove_key(root, "sliderValues");
        return Write(document.get(), a_error);
    }

    std::optional<std::string> CanonicalLayout(const std::string_view a_json, std::string& a_error, const std::optional<std::size_t> a_page)
    {
        const auto bindings = ReadLayout(a_json, a_error);
        if (!a_error.empty()) return std::nullopt;
        for (const auto& binding : bindings)
            if ((!a_page || binding.page == *a_page) && binding.id != binding.valueID)
            {
                a_error = DuplicateSliderIDError;
                return std::nullopt;
            }
        const auto source = Parse(a_json);
        Mutable document(source ? yyjson_doc_mut_copy(source.get(), nullptr) : nullptr);
        if (!document) return std::nullopt;
        auto* pages = yyjson_mut_obj_get(yyjson_mut_doc_get_root(document.get()), "pages");
        for (const auto& binding : bindings)
        {
            if (binding.moduleSlider || (a_page && binding.page != *a_page)) continue;
            auto* page = yyjson_mut_arr_get(pages, binding.page);
            auto* module = yyjson_mut_arr_get(yyjson_mut_obj_get(page, "modules"), binding.module);
            yyjson_mut_obj_put(module, yyjson_mut_str(document.get(), "id"), yyjson_mut_strcpy(document.get(), binding.id.c_str()));
        }
        return Write(document.get(), a_error);
    }

    std::optional<std::string> DraftLayout(const std::string_view a_json, std::string& a_error)
    {
        const auto bindings = ReadLayout(a_json, a_error);
        if (!a_error.empty()) return std::nullopt;
        const auto source = Parse(a_json);
        Mutable document(yyjson_doc_mut_copy(source.get(), nullptr));
        auto* pages = yyjson_mut_obj_get(yyjson_mut_doc_get_root(document.get()), "pages");
        std::unordered_set<std::string> ids;
        for (const auto& binding : bindings)
        {
            auto* page = yyjson_mut_arr_get(pages, binding.page);
            auto* module = yyjson_mut_arr_get(yyjson_mut_obj_get(page, "modules"), binding.module);
            auto* id = yyjson_mut_obj_get(module, "id");
            if (yyjson_mut_is_str(id)) ids.insert(yyjson_mut_get_str(id));
        }
        for (const auto& binding : bindings)
        {
            auto* page = yyjson_mut_arr_get(pages, binding.page);
            auto* module = yyjson_mut_arr_get(yyjson_mut_obj_get(page, "modules"), binding.module);
            if (yyjson_mut_is_str(yyjson_mut_obj_get(module, "id"))) continue;
            auto id = binding.controlID;
            for (std::size_t suffix = 1; ids.contains(id); ++suffix) id = binding.controlID + "_new" + std::to_string(suffix);
            ids.insert(id);
            yyjson_mut_obj_put(module, yyjson_mut_str(document.get(), "id"), yyjson_mut_strcpy(document.get(), id.c_str()));
        }
        return Write(document.get(), a_error);
    }

    std::optional<std::string> RemapPresets(const std::string_view a_json, const std::span<const Binding> a_old,
        const std::span<const Binding> a_new, std::string& a_error)
    {
        const auto source = Parse(a_json);
        if (!source) { a_error = "The preset catalog could not be parsed."; return std::nullopt; }
        Mutable document(yyjson_doc_mut_copy(source.get(), nullptr));
        auto* categories = yyjson_mut_obj_get(yyjson_mut_doc_get_root(document.get()), "categories");
        std::size_t c, categoryCount; yyjson_mut_val* category;
        yyjson_mut_arr_foreach(categories, c, categoryCount, category)
        {
            auto* presets = yyjson_mut_obj_get(category, "presets");
            std::size_t p, presetCount; yyjson_mut_val* preset;
            yyjson_mut_arr_foreach(presets, p, presetCount, preset)
            {
                auto* settings = yyjson_mut_obj_get(preset, "settings");
                if (!yyjson_mut_is_obj(settings)) continue;
                auto* serialized = yyjson_mut_val_write(settings, 0, nullptr);
                if (!serialized) return std::nullopt;
                const auto remapped = Remap(serialized, a_old, a_new, a_error);
                std::free(serialized);
                const auto parsed = remapped ? Parse(*remapped) : nullptr;
                if (!parsed) return std::nullopt;
                yyjson_mut_obj_put(preset, yyjson_mut_str(document.get(), "settings"), yyjson_val_mut_copy(document.get(), yyjson_doc_get_root(parsed.get())));
            }
        }
        return Write(document.get(), a_error);
    }

    std::optional<double> Number(const std::string_view a_json, const std::string_view a_path)
    {
        const auto document = Parse(a_json);
        return document ? Numeric(Find(yyjson_doc_get_root(document.get()), a_path)) : std::nullopt;
    }
}
