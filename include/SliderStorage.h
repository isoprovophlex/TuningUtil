#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::SliderStorage
{
    struct Target
    {
        std::string path;
        double scale = 1.0;
        bool operator==(const Target&) const = default;
    };

    struct Binding
    {
        std::size_t page = 0;
        std::size_t module = 0;
        std::string id;
        std::string valueID;
        std::string controlID;
        std::string legacyID;
        std::string legacyMap;
        std::vector<Target> targets;
        bool moduleSlider = false;
        double neutral = 1.0;
        bool operator==(const Binding&) const = default;
    };

    bool IsAdjustment(std::string_view);
    double Neutral(std::string_view);
    double Combine(std::string_view, double, double);
    double Scaled(std::string_view, double, double);
    std::vector<Binding> ReadLayout(std::string_view, std::string&);
    std::optional<std::string> Store(std::string_view, std::span<const Binding>, bool, std::string&);
    std::optional<std::string> Materialize(std::string_view, std::span<const Binding>, std::string&);
    std::optional<std::string> Stack(std::span<const std::string>, std::string&);
    std::optional<std::string> Remap(std::string_view, std::span<const Binding>, std::span<const Binding>, std::string&);
    std::optional<std::string> CanonicalLayout(std::string_view, std::string&, std::optional<std::size_t> = std::nullopt);
    std::optional<std::string> DraftLayout(std::string_view, std::string&);
    std::optional<std::string> RemapPresets(std::string_view, std::span<const Binding>, std::span<const Binding>, std::string&);
    std::optional<double> Number(std::string_view, std::string_view);
}
