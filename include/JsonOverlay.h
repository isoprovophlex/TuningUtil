#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::JsonOverlay
{
    struct ValueEntry
    {
        std::string path;
        std::string value;

        bool operator==(const ValueEntry&) const = default;
    };

    std::optional<bool> Equivalent(
        std::string_view a_left,
        std::string_view a_right,
        std::string& a_error);

    std::optional<bool> BooleanMember(
        std::string_view a_json,
        std::string_view a_name);

    std::optional<std::int64_t> IntegerMember(
        std::string_view a_json,
        std::string_view a_name);

    std::optional<std::string> SetBooleanMember(
        std::string_view a_json,
        std::string_view a_name,
        bool a_value,
        std::string& a_error);

    std::optional<std::string> SetIntegerMember(
        std::string_view a_json,
        std::string_view a_name,
        std::int64_t a_value,
        std::string& a_error);

    std::optional<std::string> Merge(
        std::string_view a_defaults,
        std::string_view a_overrides,
        std::string& a_error);

    std::optional<std::string> Overlay(
        std::string_view a_current,
        std::string_view a_changes,
        std::string& a_error);

    std::optional<std::string> Difference(
        std::string_view a_current,
        std::string_view a_defaults,
        std::string& a_error);

    std::optional<std::string> ProjectLike(
        std::string_view a_source,
        std::string_view a_schema,
        std::string& a_error);

    std::optional<std::string> ProjectPaths(
        std::string_view a_source,
        std::span<const std::string> a_paths,
        std::string& a_error);

    std::optional<std::string> RemovePaths(
        std::string_view a_source,
        std::span<const std::string> a_paths,
        std::string& a_error);

    std::optional<std::string> RemoveLike(
        std::string_view a_source,
        std::string_view a_schema,
        std::string& a_error);

    std::optional<std::vector<ValueEntry>> FlattenValues(
        std::string_view a_json,
        std::string& a_error);
}  // namespace MPL::JsonOverlay
