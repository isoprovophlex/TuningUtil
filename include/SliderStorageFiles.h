#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace MPL::SliderStorage
{
    struct DraftUpdate
    {
        std::filesystem::path path;
        std::string text;
    };

    bool PrepareDraft(const std::filesystem::path&, std::string&);
    bool CommitLayout(const std::filesystem::path&, std::string_view, const std::filesystem::path&,
        std::string&, std::optional<std::size_t> = std::nullopt,
        const std::optional<DraftUpdate>& = std::nullopt);
}
