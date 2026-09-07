#include <FileIO.h>

#include <fstream>
#include <format>
#include <system_error>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace MPL::FileIO
{
    bool WriteClosed(const std::filesystem::path& a_path, const std::string_view a_text, std::string& a_error)
    {
        a_error.clear();
        std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
        file.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
        file.close();
        if (file) return true;
        a_error = std::format("The file could not be completely written: {}", a_path.string());
        return false;
    }

    bool Replace(const std::filesystem::path& a_source, const std::filesystem::path& a_target, std::string& a_error)
    {
        a_error.clear();
        if (::MoveFileExW(a_source.c_str(), a_target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const std::error_code error(static_cast<int>(::GetLastError()), std::system_category());
        a_error = std::format("The file could not be replaced: {} | {}", a_target.string(), error.message());
        return false;
    }

    bool WriteAtomically(const std::filesystem::path& a_path, const std::string_view a_text, std::string& a_error)
    {
        auto temporary = a_path;
        temporary += ".tmp";
        if (WriteClosed(temporary, a_text, a_error) && Replace(temporary, a_path, a_error)) return true;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
}
