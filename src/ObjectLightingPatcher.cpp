#include <DetailedLogging.h>
#include <Config.h>
#include <ObjectLightingPatcher.h>
#include <RecordFilter.h>
#include <TuningUtil.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MPL::ObjectLightingPatcher
{
    namespace
    {
        struct ActiveRule
        {
            std::string key;
            RecordFilter::Resolved filter;
            double emissiveMultiplier = 1.0;
            double baseColorScale = 1.0;

            bool operator==(const ActiveRule&) const = default;
        };

        struct PropertyBaseline
        {
            float source = 0.0f;
            float applied = 0.0f;
            std::unordered_set<std::string> rules;
        };

        struct EffectPropertyBaseline : PropertyBaseline
        {
            RE::BSEffectShaderMaterial* material = nullptr;
        };

        std::vector<ActiveRule> activeRules;
        std::unordered_map<RE::BSLightingShaderProperty*, PropertyBaseline> lightingBaselines;
        std::unordered_map<RE::BSEffectShaderProperty*, EffectPropertyBaseline> effectBaselines;
        std::mutex stateLock;
        std::mutex reconciliationLock;
        std::unordered_set<RE::FormID> pendingReferences;
        bool reconciliationQueued = false;
        bool fullReconciliationRequested = false;
        bool runtimeEventsInstalled = false;
        std::atomic<std::uint64_t> reconciliationGeneration{ 0 };

        bool NearlyEqual(const float a_left, const float a_right)
        {
            const auto scale = std::max({ 1.0f, std::abs(a_left), std::abs(a_right) });
            return std::abs(a_left - a_right) <= scale * 0.00001f;
        }

        double RuleValue(
            const TuningUtil::Settings& a_settings,
            const TuningUtil::FilteredObjectLightingRule& a_rule)
        {
            if (const auto exact = a_settings.filteredObjectLightingAdjustments.find(a_rule.id);
                exact != a_settings.filteredObjectLightingAdjustments.end())
                return exact->second;
            const auto insensitive = std::ranges::find_if(
                a_settings.filteredObjectLightingAdjustments,
                [&](const auto& a_entry) { return Config::IEquals(a_entry.first, a_rule.id); });
            return insensitive != a_settings.filteredObjectLightingAdjustments.end() ?
                       insensitive->second :
                       a_rule.defaultValue;
        }

        std::vector<ActiveRule> ResolveActiveRules()
        {
            std::vector<ActiveRule> result;
            const TuningUtil::PluginFilter noPlugins;
            for (const auto& profile : TuningUtil::GetProfiles())
            {
                auto profileName = profile.name;
                const auto& settings = TuningUtil::GetSettings(profileName);
                if (!settings.EnableProfile) continue;

                for (const auto& rule : profile.filteredObjectLightingRules)
                {
                    auto emissiveMultiplier = 1.0;
                    auto baseColorScale = 1.0;
                    const auto value = RuleValue(settings, rule);
                    for (const auto& setting : rule.settings)
                    {
                        const auto multiplier = std::max(0.0, 1.0 + ((value - 1.0) * setting.scale));
                        if (setting.operation == TuningUtil::FilteredObjectLightingOperation::baseColorScale)
                            baseColorScale *= multiplier;
                        else
                            emissiveMultiplier *= multiplier;
                    }
                    result.push_back({
                        .key = profile.name + "\x1F" + rule.id,
                        .filter = RecordFilter::Resolve(rule.include, rule.exclude, noPlugins, noPlugins),
                        .emissiveMultiplier = emissiveMultiplier,
                        .baseColorScale = baseColorScale,
                    });
                }
            }
            return result;
        }

        std::vector<std::size_t> MatchingRules(const RE::TESBoundObject* a_baseObject)
        {
            std::vector<std::size_t> result;
            if (!a_baseObject) return result;
            for (std::size_t index = 0; index < activeRules.size(); ++index)
            {
                if (RecordFilter::Matches(a_baseObject, activeRules[index].filter)) result.push_back(index);
            }
            return result;
        }

        double MultiplierForRules(
            const std::unordered_set<std::string>& a_rules,
            const TuningUtil::FilteredObjectLightingOperation a_operation)
        {
            auto result = 1.0;
            for (const auto& rule : activeRules)
            {
                if (!a_rules.contains(rule.key)) continue;
                result *= a_operation == TuningUtil::FilteredObjectLightingOperation::baseColorScale ?
                              rule.baseColorScale :
                              rule.emissiveMultiplier;
            }
            return std::max(0.0, result);
        }

        void MarkDirty(RE::BSShaderProperty& a_property)
        {
            a_property.lastRenderPassState = (std::numeric_limits<std::int32_t>::max)();
        }

        void ApplyLightingProperty(
            RE::BSLightingShaderProperty& a_property,
            std::unordered_set<std::string> a_ruleKeys)
        {
            const auto multiplier = MultiplierForRules(
                a_ruleKeys,
                TuningUtil::FilteredObjectLightingOperation::emissiveMultiplier);
            auto baseline = lightingBaselines.find(std::addressof(a_property));
            if (baseline == lightingBaselines.end())
            {
                if (a_ruleKeys.empty() || std::abs(multiplier - 1.0) <= 0.000001) return;
                baseline = lightingBaselines.emplace(
                    std::addressof(a_property),
                    PropertyBaseline{
                        .source = a_property.emissiveMult,
                        .applied = a_property.emissiveMult,
                    }).first;
            }

            auto& captured = baseline->second;
            if (!NearlyEqual(a_property.emissiveMult, captured.applied))
            {
                captured.source = a_property.emissiveMult;
            }

            if (a_ruleKeys.empty() || std::abs(multiplier - 1.0) <= 0.000001)
            {
                if (NearlyEqual(a_property.emissiveMult, captured.applied) &&
                    !NearlyEqual(a_property.emissiveMult, captured.source))
                {
                    a_property.emissiveMult = captured.source;
                    MarkDirty(a_property);
                }
                lightingBaselines.erase(baseline);
                return;
            }

            const auto adjusted = static_cast<float>(captured.source * multiplier);
            if (!NearlyEqual(a_property.emissiveMult, adjusted))
            {
                a_property.emissiveMult = adjusted;
                MarkDirty(a_property);
            }
            captured.applied = adjusted;
            captured.rules = std::move(a_ruleKeys);
        }

        void ApplyEffectProperty(
            RE::BSEffectShaderProperty& a_property,
            std::unordered_set<std::string> a_ruleKeys)
        {
            const auto multiplier = MultiplierForRules(
                a_ruleKeys,
                TuningUtil::FilteredObjectLightingOperation::baseColorScale);
            auto* material = a_property.GetMaterial();
            auto baseline = effectBaselines.find(std::addressof(a_property));
            if (baseline == effectBaselines.end())
            {
                if (!material || a_ruleKeys.empty() || std::abs(multiplier - 1.0) <= 0.000001) return;
                const auto source = material->baseColorScale;
                a_property.SetMaterial(material, true);
                material = a_property.GetMaterial();
                if (!material) return;
                baseline = effectBaselines.emplace(
                    std::addressof(a_property),
                    EffectPropertyBaseline{
                        { .source = source, .applied = source },
                        material,
                    }).first;
            }

            auto& captured = baseline->second;
            if (!material)
            {
                effectBaselines.erase(baseline);
                return;
            }
            if (material != captured.material)
            {
                captured.source = material->baseColorScale;
                captured.applied = material->baseColorScale;
                captured.material = material;
            }
            else if (!NearlyEqual(material->baseColorScale, captured.applied))
            {
                captured.source = material->baseColorScale;
            }

            if (a_ruleKeys.empty() || std::abs(multiplier - 1.0) <= 0.000001)
            {
                if (NearlyEqual(material->baseColorScale, captured.applied) &&
                    !NearlyEqual(material->baseColorScale, captured.source))
                {
                    material->baseColorScale = captured.source;
                    MarkDirty(a_property);
                }
                effectBaselines.erase(baseline);
                return;
            }

            const auto adjusted = static_cast<float>(captured.source * multiplier);
            if (!NearlyEqual(material->baseColorScale, adjusted))
            {
                material->baseColorScale = adjusted;
                MarkDirty(a_property);
            }
            captured.applied = adjusted;
            captured.rules = std::move(a_ruleKeys);
        }

        void ApplyReference(RE::TESObjectREFR* a_reference)
        {
            auto* root = a_reference ? a_reference->GetCurrent3D() : nullptr;
            const auto matchingRules = MatchingRules(a_reference ? a_reference->GetBaseObject() : nullptr);
            if (!root || matchingRules.empty()) return;

            RE::BSVisit::TraverseScenegraphGeometries(
                root,
                [&](RE::BSGeometry* a_geometry)
                {
                    auto* shaderProperty = a_geometry ?
                                               a_geometry->GetGeometryRuntimeData().shaderProperty.get() :
                                               nullptr;
                    auto* lightingProperty = shaderProperty ?
                                                 netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty) :
                                                 nullptr;
                    auto* effectProperty = shaderProperty ?
                                               netimmerse_cast<RE::BSEffectShaderProperty*>(shaderProperty) :
                                               nullptr;
                    if (!shaderProperty ||
                        !shaderProperty->flags.any(
                            RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance))
                        return RE::BSVisit::BSVisitControl::kContinue;

                    std::unordered_set<std::string> ruleKeys;
                    if (lightingProperty)
                    {
                        if (const auto existing = lightingBaselines.find(lightingProperty);
                            existing != lightingBaselines.end())
                            ruleKeys = existing->second.rules;
                    }
                    else if (effectProperty)
                    {
                        if (const auto existing = effectBaselines.find(effectProperty);
                            existing != effectBaselines.end())
                            ruleKeys = existing->second.rules;
                    }
                    else
                    {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    for (const auto index : matchingRules) ruleKeys.insert(activeRules[index].key);
                    if (lightingProperty)
                        ApplyLightingProperty(*lightingProperty, std::move(ruleKeys));
                    else
                        ApplyEffectProperty(*effectProperty, std::move(ruleKeys));
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        void ReconcileLoadedReferences()
        {
            auto* tes = RE::TES::GetSingleton();
            if (!tes) return;

            std::unordered_map<RE::BSLightingShaderProperty*, std::unordered_set<std::string>> lightingPropertyRules;
            std::unordered_map<RE::BSEffectShaderProperty*, std::unordered_set<std::string>> effectPropertyRules;
            std::unordered_set<RE::BSLightingShaderProperty*> loadedLightingProperties;
            std::unordered_set<RE::BSEffectShaderProperty*> loadedEffectProperties;
            std::size_t matchedReferences = 0;
            tes->ForEachReference(
                [&](RE::TESObjectREFR* a_reference)
                {
                    auto* root = a_reference ? a_reference->GetCurrent3D() : nullptr;
                    if (!root) return RE::BSContainer::ForEachResult::kContinue;
                    const auto matchingRules = MatchingRules(a_reference->GetBaseObject());
                    if (!matchingRules.empty()) ++matchedReferences;
                    RE::BSVisit::TraverseScenegraphGeometries(
                        root,
                        [&](RE::BSGeometry* a_geometry)
                        {
                            auto* shaderProperty = a_geometry ?
                                                       a_geometry->GetGeometryRuntimeData().shaderProperty.get() :
                                                       nullptr;
                            auto* lightingProperty = shaderProperty ?
                                                         netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty) :
                                                         nullptr;
                            auto* effectProperty = shaderProperty ?
                                                       netimmerse_cast<RE::BSEffectShaderProperty*>(shaderProperty) :
                                                       nullptr;
                            if (lightingProperty)
                                loadedLightingProperties.insert(lightingProperty);
                            else if (effectProperty)
                                loadedEffectProperties.insert(effectProperty);
                            else
                                return RE::BSVisit::BSVisitControl::kContinue;
                            if (!shaderProperty->flags.any(
                                    RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance))
                                return RE::BSVisit::BSVisitControl::kContinue;
                            auto& keys = lightingProperty ?
                                             lightingPropertyRules[lightingProperty] :
                                             effectPropertyRules[effectProperty];
                            for (const auto index : matchingRules) keys.insert(activeRules[index].key);
                            return RE::BSVisit::BSVisitControl::kContinue;
                        });
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            for (auto* property : loadedLightingProperties)
            {
                auto rules = lightingPropertyRules.find(property);
                ApplyLightingProperty(
                    *property,
                    rules != lightingPropertyRules.end() ?
                        std::move(rules->second) :
                        std::unordered_set<std::string>{});
            }
            for (auto* property : loadedEffectProperties)
            {
                auto rules = effectPropertyRules.find(property);
                ApplyEffectProperty(
                    *property,
                    rules != effectPropertyRules.end() ?
                        std::move(rules->second) :
                        std::unordered_set<std::string>{});
            }
            std::erase_if(lightingBaselines, [&](const auto& a_entry)
                { return !loadedLightingProperties.contains(a_entry.first); });
            std::erase_if(effectBaselines, [&](const auto& a_entry)
                { return !loadedEffectProperties.contains(a_entry.first); });
            DetailedLogging::Info(
                "[Object Effect Lighting] apply | rules={} | references={} | lightingProperties={} | effectProperties={}",
                activeRules.size(),
                matchedReferences,
                lightingPropertyRules.size(),
                effectPropertyRules.size());
        }

        void QueueReconciliation(RE::TESObjectREFR* a_reference, const bool a_fullRefresh)
        {
            if (!a_fullRefresh &&
                (!a_reference || !a_reference->GetBaseObject() || !a_reference->GetFormID()))
                return;

            const auto generation = reconciliationGeneration.load(std::memory_order_acquire);
            {
                std::scoped_lock lock(reconciliationLock);
                if (a_reference && a_reference->GetFormID())
                    pendingReferences.insert(a_reference->GetFormID());
                fullReconciliationRequested |= a_fullRefresh;
                if (reconciliationQueued) return;
                reconciliationQueued = true;
            }

            auto task = [generation]()
            {
                if (generation != reconciliationGeneration.load(std::memory_order_acquire)) return;
                std::vector<RE::FormID> references;
                bool fullRefresh = false;
                {
                    std::scoped_lock lock(reconciliationLock);
                    if (generation != reconciliationGeneration.load(std::memory_order_relaxed)) return;
                    references.assign(pendingReferences.begin(), pendingReferences.end());
                    pendingReferences.clear();
                    fullRefresh = std::exchange(fullReconciliationRequested, false);
                    reconciliationQueued = false;
                }
                std::scoped_lock lock(stateLock);
                if (fullRefresh)
                {
                    ReconcileLoadedReferences();
                    return;
                }
                for (const auto formID : references)
                {
                    ApplyReference(RE::TESForm::LookupByID<RE::TESObjectREFR>(formID));
                }
            };
            if (auto* taskInterface = SKSE::GetTaskInterface())
            {
                taskInterface->AddTask(std::move(task));
            }
            else
            {
                std::scoped_lock lock(reconciliationLock);
                pendingReferences.clear();
                fullReconciliationRequested = false;
                reconciliationQueued = false;
            }
        }

        class ObjectLoadedEventSink final :
            public RE::BSTEventSink<RE::TESObjectLoadedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESObjectLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
            {
                if (!a_event) return RE::BSEventNotifyControl::kContinue;
                if (a_event->loaded)
                {
                    QueueReconciliation(
                        RE::TESForm::LookupByID<RE::TESObjectREFR>(a_event->formID),
                        false);
                }
                else
                {
                    QueueReconciliation(nullptr, true);
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        ObjectLoadedEventSink objectLoadedEventSink;
    }  // namespace

    void InstallRuntimeEvents()
    {
        if (runtimeEventsInstalled) return;
        auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
        if (!eventSource)
        {
            logger::warn("[Object Effect Lighting] object load events=unregistered");
            return;
        }
        eventSource->AddEventSink(std::addressof(objectLoadedEventSink));
        runtimeEventsInstalled = true;
        logger::info("[Object Effect Lighting] object load events=registered");
    }

    void ApplyAllSettings()
    {
        auto rules = ResolveActiveRules();
        std::scoped_lock lock(stateLock);
        if (rules == activeRules) return;
        activeRules = std::move(rules);
        ReconcileLoadedReferences();
    }

    void ResetReferenceTracking()
    {
        ++reconciliationGeneration;
        {
            std::scoped_lock lock(reconciliationLock);
            pendingReferences.clear();
            fullReconciliationRequested = false;
            reconciliationQueued = false;
        }
        std::scoped_lock lock(stateLock);
        lightingBaselines.clear();
        effectBaselines.clear();
    }
}  // namespace MPL::ObjectLightingPatcher
