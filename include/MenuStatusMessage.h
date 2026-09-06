#pragma once

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace MPL::MenuStatus
{
    inline constexpr std::array WarningColor{ 1.0f, 0.647f, 0.0f, 1.0f };

    struct Message
    {
        std::string text;
        bool warning = false;

        Message WithDetail(const std::string_view a_detail) const
        {
            auto result = *this;
            if (!a_detail.empty())
            {
                result.text += " ";
                result.text += a_detail;
            }
            return result;
        }
    };

    inline Message FromKey(const std::string_view a_key, std::string a_text)
    {
        const auto warning = a_key.ends_with("Failure") ||
                             a_key == "weatherLockUnavailable" ||
                             a_key == "pageHasNoSettings" ||
                             a_key == "settingOverrideChanged";
        return { std::move(a_text), warning };
    }

    struct TimedMessage
    {
        TimedMessage& operator=(Message a_value)
        {
            value = std::move(a_value);
            changedAt = std::chrono::steady_clock::now();
            return *this;
        }

        void SetWarning(std::string a_text) { *this = Message{ std::move(a_text), true }; }
        bool empty() const { return value.text.empty(); }
        const char* c_str() const { return value.text.c_str(); }
        void clear() { value = {}; }

        Message value;
        std::chrono::steady_clock::time_point changedAt{};
    };
}
