#include <DetailedLogging.h>
#include <Config.h>
#include <ObjectLightingPatcher.h>
#include <ObjectLightingTracking.h>
#include <ObjectShaderCatalog.h>
#include <RecordFilter.h>
#include <TuningUtil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
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
            std::optional<RecordFilter::Resolved> xemiFilter;
            ObjectShaderCatalog::Capability shaders = ObjectShaderCatalog::Capability::none;
            double emissiveMultiplier = 1.0;
            double baseColorScale = 1.0;

            bool operator==(const ActiveRule&) const = default;
        };

        struct PropertyBaseline
        {
            float source = 0.0f;
            float applied = 0.0f;
        };

        struct EffectPropertyBaseline : PropertyBaseline
        {
            RE::BSEffectShaderMaterial* material = nullptr;
        };

        using ObjectLightingTracking::RuleKeys;

        struct BaseObjectMatch
        {
            RuleKeys rules;
            bool dynamic = false;
        };

        std::vector<ActiveRule> activeRules;
        std::unordered_map<RE::FormID, BaseObjectMatch> baseObjectMatches;
        std::unordered_set<RE::FormID> referencesAwaiting3D;
        ObjectLightingTracking::ReferenceTracker<RE::NiPointer<RE::BSShaderProperty>> trackedReferences;
        std::unordered_map<RE::BSLightingShaderProperty*, PropertyBaseline> lightingBaselines;
        std::unordered_map<RE::BSEffectShaderProperty*, EffectPropertyBaseline> effectBaselines;
        std::mutex stateLock;
        std::mutex reconciliationLock;
        std::unordered_map<RE::FormID, bool> pendingReferences;
        std::unordered_set<RE::FormID> pendingCells;
        bool reconciliationQueued = false;
        bool runtimeEventsInstalled = false;
        std::atomic<bool> hasActiveRules{ false };
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

        std::optional<RecordFilter::Resolved> ResolveXemiFilter(
            const TuningUtil::WeatherFilter& a_include, const TuningUtil::WeatherFilter& a_exclude)
        {
            if (a_include.formIDs.empty() && a_include.contains.empty() &&
                a_exclude.formIDs.empty() && a_exclude.contains.empty()) return std::nullopt;
            const TuningUtil::PluginFilter noPlugins;
            auto filter = RecordFilter::Resolve(a_include, a_exclude, noPlugins, noPlugins);
            filter.requireIncludedRecordMatch = !a_include.formIDs.empty() || !a_include.contains.empty();
            return filter;
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
                    auto shaders = ObjectShaderCatalog::Capability::none;
                    const auto value = RuleValue(settings, rule);
                    for (const auto& setting : rule.settings)
                    {
                        const auto multiplier = std::max(0.0, 1.0 + ((value - 1.0) * setting.scale));
                        if (setting.operation == TuningUtil::FilteredObjectLightingOperation::baseColorScale)
                        {
                            baseColorScale *= multiplier;
                            shaders = shaders | ObjectShaderCatalog::Capability::effect;
                        }
                        else
                        {
                            emissiveMultiplier *= multiplier;
                            shaders = shaders | ObjectShaderCatalog::Capability::lighting;
                        }
                    }
                    result.push_back({
                        .key = profile.name + "\x1F" + rule.id,
                        .filter = RecordFilter::Resolve(rule.include, rule.exclude, noPlugins, noPlugins),
                        .xemiFilter = ResolveXemiFilter(rule.xemiInclude, rule.xemiExclude),
                        .shaders = shaders,
                        .emissiveMultiplier = emissiveMultiplier,
                        .baseColorScale = baseColorScale,
                    });
                }
            }
            return result;
        }

        ObjectShaderCatalog::Capability BaseShaders(RE::TESBoundObject* a_baseObject)
        {
            auto* model = a_baseObject ? skyrim_cast<RE::TESModel*>(a_baseObject) : nullptr;
            const auto* path = model ? model->GetModel() : nullptr;
            return path ? ObjectShaderCatalog::Get(path) : ObjectShaderCatalog::Capability::none;
        }

        const RuleKeys& MatchingRules(RE::TESBoundObject* a_baseObject)
        {
            static const RuleKeys empty;
            if (!a_baseObject || activeRules.empty()) return empty;
            const auto [entry, inserted] = baseObjectMatches.try_emplace(a_baseObject->GetFormID());
            if (inserted)
            {
                entry->second.dynamic = a_baseObject->IsDynamicForm();
                for (const auto& rule : activeRules)
                {
                    if (RecordFilter::Matches(a_baseObject, rule.filter) &&
                        (!rule.xemiFilter || ObjectShaderCatalog::Has(BaseShaders(a_baseObject), rule.shaders)))
                        entry->second.rules.insert(rule.key);
                }
            }
            return entry->second.rules;
        }

        void RebuildBaseObjectMatches()
        {
            const auto started = std::chrono::steady_clock::now();
            baseObjectMatches.clear();
            if (activeRules.empty()) return;

            std::vector<RE::TESBoundObject*> objects;
            {
                const auto& [forms, lock] = RE::TESForm::GetAllForms();
                const RE::BSReadLockGuard guard{ lock };
                if (forms)
                {
                    for (const auto& [formID, form] : *forms)
                    {
                        if (auto* object = form ? form->As<RE::TESBoundObject>() : nullptr)
                            objects.push_back(object);
                    }
                }
            }
            baseObjectMatches.reserve(objects.size());
            std::size_t matched = 0;
            for (auto* object : objects)
            {
                if (!MatchingRules(object).empty()) ++matched;
            }
            DetailedLogging::Info(
                "[Object Effect Lighting] base object index | objects={} | matched={} | ms={:.3f}",
                objects.size(), matched,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
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
        }

        void ApplyTrackedProperty(RE::BSShaderProperty& a_property, RuleKeys a_rules)
        {
            if (!a_property.flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance))
                a_rules.clear();
            if (auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(std::addressof(a_property)))
                ApplyLightingProperty(*lighting, std::move(a_rules));
            else if (auto* effect = netimmerse_cast<RE::BSEffectShaderProperty*>(std::addressof(a_property)))
                ApplyEffectProperty(*effect, std::move(a_rules));
        }

        void TrackReference(RE::TESObjectREFR* a_reference)
        {
            if (!a_reference) return;
            const auto formID = a_reference->GetFormID();
            const auto& baseRules = MatchingRules(a_reference->GetBaseObject());
            const auto* emittance = baseRules.empty() ? nullptr :
                a_reference->extraList.GetByType<RE::ExtraEmittanceSource>();
            const auto* source = emittance ? emittance->source : nullptr;
            const auto rules = ObjectLightingTracking::FilterReferenceRules(baseRules, [&](const auto& key)
            {
                const auto rule = std::ranges::find(activeRules, key, &ActiveRule::key);
                return rule != activeRules.end() &&
                    (!rule->xemiFilter || RecordFilter::Matches(source, *rule->xemiFilter));
            });
            auto* root = rules.empty() ? nullptr : a_reference->GetCurrent3D();
            if (!root)
            {
                if (rules.empty())
                    referencesAwaiting3D.erase(formID);
                else
                    referencesAwaiting3D.insert(formID);
                trackedReferences.Remove(formID);
                return;
            }
            referencesAwaiting3D.erase(formID);

            std::vector<RE::NiPointer<RE::BSShaderProperty>> properties;
            RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry)
            {
                auto* property = a_geometry ? a_geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
                if (property &&
                    property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kExternalEmittance) &&
                    (netimmerse_cast<RE::BSLightingShaderProperty*>(property) ||
                        netimmerse_cast<RE::BSEffectShaderProperty*>(property)))
                    properties.emplace_back(property);
                return RE::BSVisit::BSVisitControl::kContinue;
            });
            trackedReferences.Update(formID, properties, rules);
        }

        void ReconcileLoadedReferences()
        {
            referencesAwaiting3D.clear();
            trackedReferences.RemoveAll();
            if (auto* tes = RE::TES::GetSingleton(); tes && !activeRules.empty())
            {
                tes->ForEachReference([](RE::TESObjectREFR* a_reference)
                {
                    TrackReference(a_reference);
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            trackedReferences.Flush(ApplyTrackedProperty);
            DetailedLogging::Info(
                "[Object Effect Lighting] rebuild tracking | rules={} | references={} | properties={}",
                activeRules.size(), trackedReferences.ReferenceCount(), trackedReferences.PropertyCount());
        }

        void RefreshPendingReferences()
        {
            const auto pending = std::exchange(referencesAwaiting3D, {});
            for (const auto formID : pending)
            {
                if (auto* reference = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID))
                    TrackReference(reference);
                else
                    trackedReferences.Remove(formID);
            }
        }

        void QueueReconciliation(
            const RE::FormID a_referenceID,
            const bool a_loaded,
            const RE::FormID a_cellID = 0)
        {
            if ((!a_referenceID && !a_cellID) || !hasActiveRules.load(std::memory_order_acquire)) return;

            std::uint64_t generation;
            {
                std::scoped_lock lock(reconciliationLock);
                generation = reconciliationGeneration.load(std::memory_order_relaxed);
                if (a_referenceID) pendingReferences.insert_or_assign(a_referenceID, a_loaded);
                if (a_cellID) pendingCells.insert(a_cellID);
                if (reconciliationQueued) return;
                reconciliationQueued = true;
            }

            auto task = [generation]()
            {
                std::scoped_lock lock(stateLock);
                std::unordered_map<RE::FormID, bool> references;
                std::unordered_set<RE::FormID> cells;
                {
                    std::scoped_lock lock(reconciliationLock);
                    if (generation != reconciliationGeneration.load(std::memory_order_relaxed)) return;
                    references.swap(pendingReferences);
                    cells.swap(pendingCells);
                    reconciliationQueued = false;
                }
                if (generation != reconciliationGeneration.load(std::memory_order_acquire) || activeRules.empty()) return;
                const auto started = std::chrono::steady_clock::now();
                for (const auto& [formID, loaded] : references)
                {
                    auto* reference = loaded ? RE::TESForm::LookupByID<RE::TESObjectREFR>(formID) : nullptr;
                    if (reference)
                        TrackReference(reference);
                    else
                    {
                        referencesAwaiting3D.erase(formID);
                        trackedReferences.Remove(formID);
                    }
                }
                for (const auto cellID : cells)
                {
                    auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(cellID);
                    if (!cell) continue;
                    std::vector<RE::NiPointer<RE::TESObjectREFR>> cellReferences;
                    {
                        auto& runtime = cell->GetRuntimeData();
                        const RE::BSSpinLockGuard guard{ runtime.spinLock };
                        cellReferences.assign(runtime.references.begin(), runtime.references.end());
                    }
                    for (const auto& reference : cellReferences)
                    {
                        if (!reference) continue;
                        const auto event = references.find(reference->GetFormID());
                        if (event == references.end() || event->second) TrackReference(reference.get());
                    }
                }
                trackedReferences.Flush(ApplyTrackedProperty);
                DetailedLogging::Info(
                    "[Object Effect Lighting] object events | events={} | cells={} | references={} | properties={} | awaiting3D={} | ms={:.3f}",
                    references.size(), cells.size(), trackedReferences.ReferenceCount(), trackedReferences.PropertyCount(),
                    referencesAwaiting3D.size(),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            };
            if (auto* taskInterface = SKSE::GetTaskInterface())
            {
                taskInterface->AddTask(std::move(task));
            }
            else
            {
                std::scoped_lock lock(reconciliationLock);
                if (generation != reconciliationGeneration.load(std::memory_order_relaxed)) return;
                pendingReferences.clear();
                pendingCells.clear();
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
                if (a_event) QueueReconciliation(a_event->formID, a_event->loaded);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        ObjectLoadedEventSink objectLoadedEventSink;

        // Object-loaded notifications can precede 3D attachment.
        class ReferenceAttachedEventSink final : public RE::BSTEventSink<RE::TESCellAttachDetachEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellAttachDetachEvent* a_event,
                RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
            {
                if (a_event && a_event->reference)
                    QueueReconciliation(a_event->reference->GetFormID(), a_event->attached);
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        class CellFullyLoadedEventSink final : public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESCellFullyLoadedEvent* a_event,
                RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
            {
                if (a_event && a_event->cell) QueueReconciliation(0, true, a_event->cell->GetFormID());
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        ReferenceAttachedEventSink referenceAttachedEventSink;
        CellFullyLoadedEventSink cellFullyLoadedEventSink;
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
        eventSource->AddEventSink(std::addressof(referenceAttachedEventSink));
        eventSource->AddEventSink(std::addressof(cellFullyLoadedEventSink));
        runtimeEventsInstalled = true;
        logger::info("[Object Effect Lighting] load/attach/cell-ready events=registered");
    }

    void ApplyAllSettings()
    {
        auto rules = ResolveActiveRules();
        std::scoped_lock lock(stateLock);
        if (rules == activeRules) return;
        const auto filtersChanged = !std::ranges::equal(rules, activeRules, [](const auto& a_left, const auto& a_right)
            { return a_left.key == a_right.key && a_left.filter == a_right.filter &&
                a_left.xemiFilter == a_right.xemiFilter && a_left.shaders == a_right.shaders; });
        activeRules = std::move(rules);
        hasActiveRules.store(!activeRules.empty(), std::memory_order_release);
        if (filtersChanged)
        {
            RebuildBaseObjectMatches();
            ReconcileLoadedReferences();
        }
        else
        {
            RefreshPendingReferences();
            trackedReferences.ApplyAll(ApplyTrackedProperty);
            for (const auto& rule : activeRules)
                DetailedLogging::Info(
                    "[Object Effect Lighting] setting | rule={} | emissiveMultiplier={} | baseColorScale={} | references={} | properties={} | awaiting3D={}",
                    rule.key, rule.emissiveMultiplier, rule.baseColorScale,
                    trackedReferences.ReferenceCount(), trackedReferences.PropertyCount(), referencesAwaiting3D.size());
        }
    }

    void ResetReferenceTracking()
    {
        std::scoped_lock lock(stateLock, reconciliationLock);
        ++reconciliationGeneration;
        pendingReferences.clear();
        pendingCells.clear();
        referencesAwaiting3D.clear();
        reconciliationQueued = false;
        trackedReferences.RemoveAll();
        trackedReferences.Flush(ApplyTrackedProperty);
        lightingBaselines.clear();
        effectBaselines.clear();
        std::erase_if(baseObjectMatches, [](const auto& a_entry) { return a_entry.second.dynamic; });
    }

    void QueueLoadedReferenceRefresh()
    {
        if (!hasActiveRules.load(std::memory_order_acquire)) return;
        if (auto* taskInterface = SKSE::GetTaskInterface())
        {
            const auto generation = reconciliationGeneration.load(std::memory_order_acquire);
            taskInterface->AddTask([generation]()
            {
                std::scoped_lock lock(stateLock);
                if (generation != reconciliationGeneration.load(std::memory_order_acquire) || activeRules.empty()) return;
                ReconcileLoadedReferences();
            });
        }
    }

    void QueueReferenceRefresh(RE::TESObjectREFR* a_reference)
    {
        if (a_reference) QueueReconciliation(a_reference->GetFormID(), true);
    }
}  // namespace MPL::ObjectLightingPatcher
