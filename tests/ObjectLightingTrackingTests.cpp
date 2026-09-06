#include <ObjectLightingTracking.h>
#include <RecordFilterLogic.h>

#include <array>
#include <iostream>
#include <memory>
#include <random>

namespace
{
    using MPL::ObjectLightingTracking::RuleKeys;
    struct Property
    {
        double value = 1.0;
    };
    using Pointer = std::shared_ptr<Property>;
    using Tracker = MPL::ObjectLightingTracking::ReferenceTracker<Pointer>;

    bool Expect(const bool a_condition, const char* a_message)
    {
        if (!a_condition) std::cerr << "FAILED: " << a_message << '\n';
        return a_condition;
    }
}

int main()
{
    bool passed = true;
    struct RecordFilter
    {
        std::unordered_set<std::uint32_t> includedFormIDs;
        std::unordered_set<std::uint32_t> excludedFormIDs;
        std::vector<std::string> includedEditorIDFragments;
        std::vector<std::string> excludedEditorIDFragments;
        bool requireIncludedRecordMatch = false;
    };
    RecordFilter interiorFilter{ .includedEditorIDFragments = { "fx" }, .excludedEditorIDFragments = { "invert" } };
    const auto matches = [&](const std::uint32_t a_formID, const std::string_view a_editorID)
    {
        return MPL::RecordFilterLogic::MatchesRecord(a_formID, interiorFilter,
            [&] { return std::string(a_editorID); });
    };
    passed &= Expect(!matches(0x8282B, "FXLightRegionInvertLightsWhiterun"),
        "an XEMI containing both fx and invert must be excluded");
    passed &= Expect(matches(1, "Helios_WeatherFx") && !matches(2, "WeatherWhiterun"),
        "XEMI inclusions match case-insensitively without accepting unrelated regions");
    interiorFilter.includedFormIDs.insert(0x8282B);
    passed &= Expect(!matches(0x8282B, "FXLightRegionInvertLightsWhiterun"),
        "an exact XEMI inclusion cannot override a text exclusion");
    interiorFilter.excludedFormIDs.insert(1);
    passed &= Expect(!matches(1, "Helios_WeatherFx"),
        "an exact XEMI exclusion overrides a text inclusion");
    interiorFilter.excludedFormIDs.insert(0x8282B);
    passed &= Expect(!matches(0x8282B, ""),
        "an exact exclusion overrides an exact inclusion even without an EditorID");
    interiorFilter = {};
    passed &= Expect(matches(1, ""), "an empty record filter accepts records without EditorIDs");
    interiorFilter.requireIncludedRecordMatch = true;
    passed &= Expect(!matches(1, ""), "unresolved required XEMI inclusions must not become a match-all filter");
    interiorFilter = { .includedEditorIDFragments = { "fx" }, .excludedEditorIDFragments = { "invert" } };
    std::size_t editorIDLookups = 0;
    passed &= Expect(MPL::RecordFilterLogic::MatchesRecord(1, interiorFilter, [&]
        {
            ++editorIDLookups;
            return std::string("Helios_WeatherFx");
        }) && editorIDLookups == 1, "inclusions and exclusions share one EditorID lookup");
    Tracker tracker;
    auto shared = std::make_shared<Property>();
    auto unrelated = std::make_shared<Property>();
    std::size_t applications = 0;
    std::unordered_map<std::string, double> values{ { "a", 2.0 }, { "b", 3.0 }, { "c", 5.0 } };
    const auto apply = [&](Property& a_property, const RuleKeys& a_rules)
    {
        ++applications;
        a_property.value = 1.0;
        for (const auto& rule : a_rules) a_property.value *= values.at(rule);
    };

    tracker.Update(1, { shared, shared }, { "a" });
    tracker.Update(2, { shared }, { "a", "b" });
    tracker.Update(3, { unrelated }, { "c" });
    tracker.Flush(apply);
    passed &= Expect(shared->value == 6.0, "shared rules apply once even with duplicate geometries and references");
    passed &= Expect(applications == 2 && tracker.PropertyCount() == 2, "each affected property is applied once per batch");

    applications = 0;
    tracker.Remove(1);
    tracker.Flush(apply);
    passed &= Expect(shared->value == 6.0 && unrelated->value == 5.0 && applications == 1,
        "unloading one contributor preserves other contributors and does not touch unrelated properties");
    tracker.Remove(999);
    tracker.Flush(apply);
    passed &= Expect(applications == 1, "unmatched unloads do no property work");

    tracker.Update(2, { shared }, { "a", "b" });
    tracker.Update(2, { shared }, { "a", "b" });
    tracker.Flush(apply);
    tracker.Remove(2);
    tracker.Flush(apply);
    passed &= Expect(shared->value == 1.0 && tracker.ReferenceCount() == 1 && tracker.PropertyCount() == 1,
        "repeated load events do not leave stale contributions after unload");

    tracker.Update(4, { shared }, { "a" });
    tracker.Flush(apply);
    values["a"] = 4.0;
    applications = 0;
    tracker.ApplyAll(apply);
    passed &= Expect(shared->value == 4.0 && applications == 2,
        "slider value changes use already tracked properties");
    values["a"] = 1.0;
    tracker.ApplyAll(apply);
    passed &= Expect(shared->value == 1.0 && tracker.ReferenceCount() == 2,
        "a neutral multiplier restores brightness without losing reference tracking");
    values["a"] = 4.0;
    tracker.ApplyAll(apply);
    passed &= Expect(shared->value == 4.0, "leaving the neutral value does not need another mesh scan");

    tracker.RemoveAll();
    tracker.Update(4, { shared }, { "b" });
    applications = 0;
    tracker.Flush(apply);
    passed &= Expect(shared->value == 3.0 && unrelated->value == 1.0 && applications == 2,
        "filter changes restore removed properties and apply new rules in one batch");
    tracker.Update(4, { unrelated }, { "a" });
    tracker.Flush(apply);
    passed &= Expect(shared->value == 1.0 && unrelated->value == 4.0,
        "replacing a reference mesh restores its previous property");
    tracker.Update(4, { unrelated }, {});
    tracker.Flush(apply);
    passed &= Expect(unrelated->value == 1.0 && tracker.ReferenceCount() == 0 && tracker.PropertyCount() == 0,
        "removing all matching rules restores originals and releases tracking");

    std::weak_ptr<Property> lifetime;
    {
        auto temporary = std::make_shared<Property>();
        lifetime = temporary;
        tracker.Update(5, { temporary }, { "a" });
        tracker.Flush(apply);
    }
    passed &= Expect(!lifetime.expired(), "tracked shader properties remain valid until deferred cleanup");
    tracker.Remove(5);
    bool restoredBeforeRelease = false;
    tracker.Flush([&](Property& a_property, const RuleKeys& a_rules)
    {
        apply(a_property, a_rules);
        restoredBeforeRelease = !lifetime.expired() && a_property.value == 1.0;
    });
    passed &= Expect(restoredBeforeRelease && lifetime.expired(), "unloaded properties are restored before the last owning pointer is released");

    tracker.Update(6, {}, { "a" });
    tracker.Flush(apply);
    passed &= Expect(tracker.ReferenceCount() == 0, "a load notification before 3D is ready has no shader properties yet");
    tracker.Update(6, { shared }, { "a" });
    tracker.ApplyAll(apply);
    passed &= Expect(shared->value == 4.0 && tracker.ReferenceCount() == 1,
        "a late attachment or pending-3D retry starts applying the current slider value");
    tracker.Remove(6);
    tracker.ApplyAll(apply);
    passed &= Expect(shared->value == 1.0 && tracker.PropertyCount() == 0,
        "a slider update also restores and releases properties removed by a pending-3D retry");

    const RuleKeys baseRules{ "a", "b", "c" };
    const auto matchesRegion = [&](const std::uint32_t a_region)
    {
        return MPL::ObjectLightingTracking::FilterReferenceRules(baseRules, [&](const auto& a_rule)
        {
            if (a_rule == "c") return true;
            return a_region != 0 && (a_rule == "a" ? a_region == 100 : a_region == 200);
        });
    };
    tracker.Update(7, { shared }, matchesRegion(100));
    tracker.Update(8, { unrelated }, matchesRegion(200));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 20.0 && unrelated->value == 15.0,
        "references of the same base object can select different rules by XEMI region");
    passed &= Expect(baseRules == RuleKeys{ "a", "b", "c" },
        "per-reference matching must not modify the cached Base Object matches");
    tracker.Update(7, { shared }, matchesRegion(200));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 15.0 && unrelated->value == 15.0,
        "a changed XEMI region replaces the reference's old contribution instead of stacking stale rules");
    tracker.Update(7, { shared }, matchesRegion(0));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 5.0,
        "missing XEMI removes region-filtered rules while preserving sliders with no XEMI filter");
    const auto noMatches = MPL::ObjectLightingTracking::FilterReferenceRules(RuleKeys{ "a", "b" },
        [](const auto&) { return false; });
    tracker.Update(7, { shared }, noMatches);
    tracker.Flush(apply);
    passed &= Expect(shared->value == 1.0,
        "a newly excluded XEMI source restores the reference's original property");
    std::size_t sourceChecks = 0;
    const auto unmatchedBase = MPL::ObjectLightingTracking::FilterReferenceRules({}, [&](const auto&)
    {
        ++sourceChecks;
        return true;
    });
    passed &= Expect(unmatchedBase.empty() && sourceChecks == 0,
        "an XEMI inclusion must not bypass a Base Object exclusion");
    tracker.RemoveAll();
    tracker.Flush(apply);

    const RecordFilter inside{ .includedEditorIDFragments = { "fx" }, .excludedEditorIDFragments = { "invert" } };
    const RecordFilter outside{ .includedEditorIDFragments = { "invert" } };
    const std::unordered_map<std::uint32_t, std::string> sourceNames{
        { 100, "HeliosFXSunlight" }, { 200, "FXInvertWindows" }, { 300, "XemiUtilityUnrelated" }
    };
    const auto runtimeRules = [&](const std::uint32_t source)
    {
        return MPL::ObjectLightingTracking::FilterReferenceRules({ "a", "b" }, [&](const auto& key)
        {
            return source != 0 && MPL::RecordFilterLogic::MatchesRecord(
                source, key == "a" ? inside : outside, [&] { return sourceNames.at(source); });
        });
    };
    tracker.Update(10, { shared }, runtimeRules(0));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 1.0, "a reference without XEMI does not match an XEMI filter");
    tracker.Update(10, { shared }, runtimeRules(100));
    tracker.Update(11, { unrelated }, runtimeRules(200));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 4.0 && unrelated->value == 3.0,
        "runtime-added XEMI is accepted and another reference's exclusion does not exclude the entire base object");
    tracker.Update(10, { shared }, runtimeRules(300));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 1.0 && unrelated->value == 3.0,
        "a mod replacing XEMI with a nonmatching source restores only that reference");
    tracker.Update(10, { shared }, runtimeRules(200));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 3.0, "a replacement XEMI selects the new rule without stale multipliers");
    tracker.Update(10, { shared }, runtimeRules(0));
    tracker.Flush(apply);
    passed &= Expect(shared->value == 1.0 && unrelated->value == 3.0,
        "runtime removal of XEMI restores the reference without disturbing another instance");
    tracker.RemoveAll();
    tracker.Flush(apply);

    struct Reference
    {
        std::vector<Pointer> properties;
        RuleKeys rules;
    };
    std::array<Pointer, 12> properties;
    for (auto& property : properties) property = std::make_shared<Property>();
    std::unordered_map<std::uint32_t, Reference> expectedReferences;
    std::mt19937 random{ 17 };
    const std::array<std::string, 3> rules{ "a", "b", "c" };
    for (std::size_t batch = 0; batch < 500; ++batch)
    {
        for (std::size_t event = 0; event < 8; ++event)
        {
            const auto referenceID = static_cast<std::uint32_t>(random() % 32);
            if (random() % 3 == 0)
            {
                tracker.Remove(referenceID);
                expectedReferences.erase(referenceID);
            }
            else
            {
                Reference reference;
                for (std::size_t i = 0; i < 3; ++i)
                {
                    reference.properties.push_back(properties[random() % properties.size()]);
                    reference.rules.insert(rules[random() % rules.size()]);
                }
                tracker.Update(referenceID, reference.properties, reference.rules);
                expectedReferences.insert_or_assign(referenceID, std::move(reference));
            }
        }
        tracker.Flush(apply);
        for (const auto& property : properties)
        {
            RuleKeys expectedRules;
            for (const auto& [referenceID, reference] : expectedReferences)
            {
                for (const auto& candidate : reference.properties)
                {
                    if (candidate == property) expectedRules.insert(reference.rules.begin(), reference.rules.end());
                }
            }
            double expected = 1.0;
            for (const auto& rule : expectedRules) expected *= values.at(rule);
            passed &= Expect(property->value == expected, "batched load/unload results match a complete reference rescan");
        }
    }
    tracker.RemoveAll();
    tracker.Flush(apply);
    for (const auto& property : properties)
        passed &= Expect(property->value == 1.0, "save-load reset restores every tracked property");
    passed &= Expect(tracker.ReferenceCount() == 0 && tracker.PropertyCount() == 0,
        "save-load reset releases all reference and property tracking");
    return passed ? 0 : 1;
}
