#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace MPL::FileIO
{
    bool WriteClosed(const std::filesystem::path&, std::string_view, std::string&);
    bool Replace(const std::filesystem::path&, const std::filesystem::path&, std::string&);
    bool WriteAtomically(const std::filesystem::path&, std::string_view, std::string&);
}
