#pragma once

#include <regex>
#include <string>
#include <string_view>

namespace MPL::JsonFormatting
{
    inline std::string CompactLinkArrays(const std::string& a_json)
    {
        static const std::regex arrays(
            R"json(\[\s*("(?:\\.|[^"\\])*")\s*,\s*(-?[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)\s*\])json");
        return std::regex_replace(a_json, arrays, "[ $1, $2 ]");
    }

    inline std::string Pretty(const std::string_view a_json)
    {
        std::string result;
        result.reserve(a_json.size() + a_json.size() / 4);

        std::size_t indentation = 0;
        bool inString = false;
        bool escaped = false;

        const auto appendIndentation = [&]()
        {
            result.append(indentation * 2, ' ');
        };
        const auto appendNewLine = [&]()
        {
            result.push_back('\n');
            appendIndentation();
        };

        for (std::size_t index = 0; index < a_json.size(); ++index)
        {
            const auto character = a_json[index];
            if (inString)
            {
                result.push_back(character);
                if (escaped)
                {
                    escaped = false;
                }
                else if (character == '\\')
                {
                    escaped = true;
                }
                else if (character == '"')
                {
                    inString = false;
                }
                continue;
            }

            if (character == '"')
            {
                inString = true;
                result.push_back(character);
                continue;
            }
            if (character == ' ' || character == '\t' ||
                character == '\r' || character == '\n')
            {
                continue;
            }

            switch (character)
            {
            case '{':
            case '[':
            {
                result.push_back(character);
                const auto closing = character == '{' ? '}' : ']';
                const auto next = a_json.find_first_not_of(" \t\r\n", index + 1);
                if (next != a_json.npos && a_json[next] == closing)
                {
                    result.push_back(closing);
                    index = next;
                }
                else
                {
                    ++indentation;
                    appendNewLine();
                }
                break;
            }
            case '}':
            case ']':
                if (indentation > 0)
                {
                    --indentation;
                }
                appendNewLine();
                result.push_back(character);
                break;
            case ',':
                result.push_back(character);
                appendNewLine();
                break;
            case ':':
                result.append(": ");
                break;
            default:
                result.push_back(character);
                break;
            }
        }
        return CompactLinkArrays(result);
    }
}  // namespace MPL::JsonFormatting
