#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::ProfileSettings
{
    inline constexpr std::string_view kNewBaseSettings =
        R"({"tuningProfileSettings":true,"profileSettingsPriority":0})";

    struct Layer
    {
        std::filesystem::path path;
        int priority = 0;
        std::string text;
    };

    struct Defaults
    {
        std::string text{ "{}" };
        std::vector<Layer> layers;
    };

    std::optional<Defaults> Load(const std::filesystem::path&, std::string&);
}
