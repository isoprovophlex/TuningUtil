#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace MPL::ObjectLightingTracking
{
    using RuleKeys = std::unordered_set<std::string>;

    template <class MatchesReference>
    RuleKeys FilterReferenceRules(const RuleKeys& a_baseRules, const MatchesReference& a_matchesReference)
    {
        RuleKeys result;
        for (const auto& key : a_baseRules)
            if (a_matchesReference(key)) result.insert(key);
        return result;
    }

    template <class PropertyPointer>
    class ReferenceTracker
    {
    public:
        using Property = typename PropertyPointer::element_type;

        void Update(
            const std::uint32_t a_referenceID,
            const std::vector<PropertyPointer>& a_properties,
            const RuleKeys& a_rules)
        {
            Remove(a_referenceID);
            if (a_properties.empty() || a_rules.empty()) return;

            auto& reference = references[a_referenceID];
            reference.rules = a_rules;
            for (const auto& property : a_properties)
            {
                if (!property || !reference.properties.insert(property.get()).second) continue;
                auto& tracked = properties[property.get()];
                if (!tracked.property) tracked.property = property;
                for (const auto& rule : a_rules) ++tracked.ruleReferences[rule];
                dirtyProperties.insert(property.get());
            }
        }

        void Remove(const std::uint32_t a_referenceID)
        {
            const auto reference = references.find(a_referenceID);
            if (reference == references.end()) return;
            for (auto* property : reference->second.properties)
            {
                auto& counts = properties.at(property).ruleReferences;
                for (const auto& rule : reference->second.rules)
                {
                    const auto count = counts.find(rule);
                    assert(count != counts.end() && count->second != 0);
                    if (--count->second == 0) counts.erase(count);
                }
                dirtyProperties.insert(property);
            }
            references.erase(reference);
        }

        void RemoveAll()
        {
            references.clear();
            for (auto& [property, tracked] : properties)
            {
                tracked.ruleReferences.clear();
                dirtyProperties.insert(property);
            }
        }

        template <class Apply>
        void Flush(const Apply& a_apply)
        {
            for (auto* property : dirtyProperties)
            {
                const auto tracked = properties.find(property);
                assert(tracked != properties.end());
                ApplyProperty(tracked->second, a_apply);
                if (tracked->second.ruleReferences.empty()) properties.erase(tracked);
            }
            dirtyProperties.clear();
        }

        template <class Apply>
        void ApplyAll(const Apply& a_apply)
        {
            for (const auto& [property, tracked] : properties) dirtyProperties.insert(property);
            Flush(a_apply);
        }

        std::size_t ReferenceCount() const { return references.size(); }
        std::size_t PropertyCount() const { return properties.size(); }

    private:
        struct Reference
        {
            std::unordered_set<Property*> properties;
            RuleKeys rules;
        };

        struct TrackedProperty
        {
            PropertyPointer property;
            std::unordered_map<std::string, std::size_t> ruleReferences;
        };

        template <class Apply>
        static void ApplyProperty(const TrackedProperty& a_tracked, const Apply& a_apply)
        {
            RuleKeys rules;
            for (const auto& [rule, count] : a_tracked.ruleReferences) rules.insert(rule);
            a_apply(*a_tracked.property, std::move(rules));
        }

        std::unordered_map<std::uint32_t, Reference> references;
        std::unordered_map<Property*, TrackedProperty> properties;
        std::unordered_set<Property*> dirtyProperties;
    };
}
