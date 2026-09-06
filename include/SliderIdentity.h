#pragma once

#include <algorithm>
#include <cctype>
#include <span>
#include <string>
#include <string_view>

namespace MPL::SliderIdentity
{
    inline std::string Component(const std::string_view a_name)
    {
        std::string result;
        bool nextWord = false;
        for (const unsigned char character : a_name)
        {
            if (std::isspace(character) || character == '_' || character == '-')
            {
                nextWord = !result.empty();
                continue;
            }
            if (character == '.' || character == '%')
            {
                result += character == '.' ? "%2e" : "%25";
                nextWord = false;
                continue;
            }
            result += static_cast<char>(nextWord ? std::toupper(character) : std::tolower(character));
            nextWord = false;
        }
        return result;
    }

    inline std::string Make(
        const std::string_view a_page,
        const std::span<const std::string> a_dropBoxes,
        const std::string_view a_slider)
    {
        auto result = Component(a_page);
        for (const auto& box : a_dropBoxes)
        {
            if (!box.empty()) result += "." + Component(box);
        }
        return result + "." + Component(a_slider);
    }

    inline std::string ComparisonKey(std::string a_id)
    {
        std::ranges::transform(a_id, a_id.begin(), [](const unsigned char a_character)
        {
            return static_cast<char>(std::tolower(a_character));
        });
        return a_id;
    }
}
