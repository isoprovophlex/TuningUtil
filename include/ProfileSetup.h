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
        "weatherPluginOwnership",
        "weatherPluginInclusions",
        "weatherPluginExclusions",
        "exclusiveControl.weather",
        "exclusiveControl.exteriorImageSpace",
    };

    inline const std::vector<std::string> kLightingSettingPaths{
        "links.lighting",
        "enableTemplateInherit",
        "cellExclusions",
        "lightHueRanges",
        "lightingTemplateInclusions",
        "lightingTemplateExclusions",
        "lightingTemplatePluginOwnership",
        "lightingTemplatePluginInclusions",
        "lightingTemplatePluginExclusions",
        "exclusiveControl.lightingTemplates",
        "exclusiveControl.cellLighting",
        "exclusiveControl.lightingImageSpace",
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
