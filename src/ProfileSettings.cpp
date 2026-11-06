#include <ProfileSettings.h>
#include <JsonOverlay.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <yyjson.h>

namespace MPL::ProfileSettings
{
    namespace
    {
        std::string Lowercase(std::string a_text)
        {
            std::ranges::transform(a_text, a_text.begin(), [](const unsigned char a_character)
                { return static_cast<char>(std::tolower(a_character)); });
            return a_text;
        }
    }

    std::optional<Defaults> Load(const std::filesystem::path& a_directory, std::string& a_error)
    {
        a_error.clear();
        Defaults result;
        std::error_code fileError;
        for (std::filesystem::directory_iterator iterator(a_directory, fileError), end;
            iterator != end && !fileError; iterator.increment(fileError))
        {
            const auto& path = iterator->path();
            if (!iterator->is_regular_file(fileError) || Lowercase(path.extension().string()) != ".json") continue;
            const auto base = Lowercase(path.filename().string()) == "profilesettings.json";
            std::ifstream stream(path, std::ios::binary);
            std::string text(std::istreambuf_iterator<char>(stream), {});
            if (!stream || stream.bad())
            {
                a_error = std::format("Profile settings could not be read: {}", path.string());
                return std::nullopt;
            }
            constexpr std::string_view bom = "\xEF\xBB\xBF";
            if (text.starts_with(bom)) text.erase(0, bom.size());
            const std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(
                yyjson_read(text.data(), text.size(), 0), yyjson_doc_free);
            auto* root = document ? yyjson_doc_get_root(document.get()) : nullptr;
            if (!base && (!yyjson_is_obj(root) ||
                !yyjson_is_true(yyjson_obj_get(root, "tuningProfileSettings")))) continue;
            if (!yyjson_is_obj(root))
            {
                a_error = std::format("Profile settings must contain a JSON object: {}", path.string());
                return std::nullopt;
            }
            int priority = 0;
            if (auto* value = yyjson_obj_get(root, "profileSettingsPriority"))
            {
                if ((!yyjson_is_sint(value) && !yyjson_is_uint(value)) ||
                    (yyjson_is_uint(value) && yyjson_get_uint(value) > std::numeric_limits<int>::max()) ||
                    (yyjson_is_sint(value) && (yyjson_get_sint(value) < std::numeric_limits<int>::min() ||
                        yyjson_get_sint(value) > std::numeric_limits<int>::max())))
                {
                    a_error = std::format("Profile settings priority must be a 32-bit integer: {}", path.string());
                    return std::nullopt;
                }
                priority = static_cast<int>(yyjson_get_sint(value));
            }
            result.layers.push_back({ path, priority, std::move(text) });
        }
        if (fileError)
        {
            a_error = std::format("Profile settings directory could not be read: {} | {}",
                a_directory.string(), fileError.message());
            return std::nullopt;
        }
        std::ranges::sort(result.layers, [](const Layer& a_left, const Layer& a_right)
        {
            if (a_left.priority != a_right.priority) return a_left.priority < a_right.priority;
            const auto left = Lowercase(a_left.path.filename().string());
            const auto right = Lowercase(a_right.path.filename().string());
            return left != right ? left < right : a_left.path.filename() < a_right.path.filename();
        });
        for (const auto& layer : result.layers)
        {
            const auto merged = JsonOverlay::Overlay(result.text, layer.text, a_error);
            if (!merged) return std::nullopt;
            result.text = *merged;
        }
        const std::array<std::string, 2> metadata{ "tuningProfileSettings", "profileSettingsPriority" };
        auto values = JsonOverlay::RemovePaths(result.text, metadata, a_error);
        if (!values) return std::nullopt;
        result.text = std::move(*values);
        return result;
    }
}
