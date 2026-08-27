#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::PresetCatalog
{
    inline constexpr std::string_view kFileName = "presets.json";

    struct Preset
    {
        std::string name;
        std::string settings{ "{}" };
    };

    struct Category
    {
        std::string name;
        std::vector<Preset> presets;
    };

    struct Catalog
    {
        std::vector<Category> categories;
    };

    std::optional<Catalog> Parse(std::string_view, std::string&);
    std::optional<std::string> Serialize(const Catalog&, std::string&);
    std::optional<Catalog> Read(const std::filesystem::path&, std::string&);
    bool Write(const std::filesystem::path&, const Catalog&, std::string&);

    Category* FindCategory(Catalog&, std::string_view);
    const Category* FindCategory(const Catalog&, std::string_view);
    Preset* FindPreset(Category&, std::string_view);
    const Preset* FindPreset(const Category&, std::string_view);
}  // namespace MPL::PresetCatalog
