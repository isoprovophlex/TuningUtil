#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MPL::SliderCreator
{
    enum class FilterDomain
    {
        weather,
        lightingTemplate,
        baseLight,
    };

    inline constexpr std::array<std::string_view, 4> kRequiredProfileModuleKinds{
        "profileActions",
        "enableProfile",
        "profilePriority",
        "advancedToggle",
    };

    bool IsRequiredProfileModuleKind(std::string_view);

    struct Target
    {
        std::string setting;
        double scale = 1.0;
        bool ignoreLink = false;
    };

    struct Filter
    {
        std::vector<std::string> formIDs;
        std::vector<std::string> contains;
        std::vector<std::string> locationTypes;
        std::vector<std::string> multiLocationExceptions;
    };

    struct HueScales
    {
        double red = 1.0;
        double orange = 1.0;
        double yellow = 1.0;
        double green = 1.0;
        double teal = 1.0;
        double blue = 1.0;
        double magenta = 1.0;

        bool operator==(const HueScales&) const = default;
    };

    struct Definition
    {
        std::string id;
        std::string label;
        std::string tooltip;
        std::string link;
        std::string localLink;
        std::vector<Target> settings;
        std::optional<HueScales> hueScales;
        bool filtered = true;
        FilterDomain filterDomain = FilterDomain::weather;
        bool invert = false;
        bool useTimes = false;
        std::array<bool, 4> times{ true, true, true, true };
        Filter include;
        Filter exclude;
        std::optional<double> defaultValue;
        std::optional<double> minimum;
        std::optional<double> maximum;
        std::optional<double> step;
        std::optional<double> width;
        std::string format;
    };

    struct ExistingSlider
    {
        std::size_t controlIndex = 0;
        Definition definition;
    };

    struct Page
    {
        std::string title;
        bool advanced = false;
        std::vector<ExistingSlider> sliders;
    };

    std::vector<Page> Load(const std::filesystem::path&, std::string&);
    bool CreateProfile(
        const std::filesystem::path&,
        const std::string&,
        std::string&,
        const std::filesystem::path& = {});
    std::optional<std::size_t> CreatePage(
        const std::filesystem::path&,
        const std::string&,
        std::string&);
    bool AddModule(
        const std::filesystem::path&,
        std::size_t,
        const std::string&,
        const std::string&,
        const std::string&,
        bool,
        std::string&);
    bool MoveModule(
        const std::filesystem::path&,
        std::size_t,
        std::size_t,
        int,
        std::string&);
    bool RemoveModule(
        const std::filesystem::path&,
        std::size_t,
        std::size_t,
        std::string&);
    bool RenameModule(
        const std::filesystem::path&,
        std::size_t,
        std::size_t,
        const std::string&,
        std::string&);
    bool AddProfileElement(
        const std::filesystem::path&,
        const std::string&,
        const std::string&,
        std::string&);
    bool MoveProfileModule(
        const std::filesystem::path&,
        std::size_t,
        int,
        std::string&);
    bool RemoveProfileModule(
        const std::filesystem::path&,
        std::size_t,
        std::string&);
    bool RenameProfileModule(
        const std::filesystem::path&,
        std::size_t,
        const std::string&,
        std::string&);
    bool MoveProfilePage(
        const std::filesystem::path&,
        int,
        std::string&);
    bool MovePage(
        const std::filesystem::path&,
        std::size_t,
        int,
        std::string&);
    bool RenamePage(
        const std::filesystem::path&,
        std::size_t,
        const std::string&,
        std::string&);
    bool SetPageAdvanced(
        const std::filesystem::path&,
        std::size_t,
        bool,
        std::string&);
    bool RemovePage(
        const std::filesystem::path&,
        std::size_t,
        std::string&);
    bool SavePageEdits(
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::size_t,
        std::optional<std::size_t>,
        std::size_t&,
        std::string&);
    bool RestorePageEdits(
        const std::filesystem::path&,
        const std::filesystem::path&,
        std::size_t,
        std::optional<std::size_t>,
        std::string&);
    bool Save(
        const std::filesystem::path&,
        std::size_t,
        std::optional<std::size_t>,
        const Definition&,
        std::string&);
}  // namespace MPL::SliderCreator
