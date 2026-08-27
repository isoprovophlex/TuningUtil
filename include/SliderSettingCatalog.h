#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace MPL::SliderSettingCatalog
{
    enum class Domain
    {
        weather,
        lighting,
    };

    enum class FilterOperation
    {
        none,
        brightness,
        saturation,
        hueShift,
    };

    struct Entry
    {
        Domain domain;
        std::string group;
        std::string label;
        std::string path;
        std::string target;
        std::string hue;
        FilterOperation filterOperation = FilterOperation::none;
        bool linkable = false;
        bool hueScales = false;
        bool aggregate = false;
    };

    const std::vector<Entry>& Entries();
    std::vector<std::string_view> Groups(Domain);
    std::vector<const Entry*> Entries(Domain, std::string_view);
    const Entry* Find(std::string_view);
    bool IsFilteredOperation(FilterOperation);
}  // namespace MPL::SliderSettingCatalog
