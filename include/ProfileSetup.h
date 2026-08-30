#pragma once

#include <string>
#include <vector>

namespace MPL::ProfileSetup
{
    enum class Domain
    {
        weather,
        lighting,
    };

    inline const std::vector<std::string> kWeatherSettingPaths{
        "links.weather",
        "hueScales",
        "hueRanges",
        "weatherInclusions",
        "weatherExclusions",
        "pluginInclusions",
        "pluginExclusions",
    };

    inline const std::vector<std::string> kLightingSettingPaths{
        "links.interior",
        "enableTemplateInherit",
        "cellExclusions",
        "effectPointLightInclusions",
        "effectPointLightExclusions",
        "intAmbientHueScales",
        "pointLights.hueScales",
        "intHueRanges",
        "lightingTemplateInclusions",
        "lightingTemplateExclusions",
        "lightingTemplatePluginInclusions",
        "lightingTemplatePluginExclusions",
        "lightingTemplateFilter",
    };

    inline const std::vector<std::string> kSettingPaths = []
    {
        auto paths = kWeatherSettingPaths;
        paths.insert(paths.end(), kLightingSettingPaths.begin(), kLightingSettingPaths.end());
        return paths;
    }();

    inline const std::vector<std::string>& SettingPaths(const Domain a_domain)
    {
        return a_domain == Domain::weather ? kWeatherSettingPaths : kLightingSettingPaths;
    }
}  // namespace MPL::ProfileSetup
