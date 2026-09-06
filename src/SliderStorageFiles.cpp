#include <SliderStorageFiles.h>
#include <SliderStorage.h>
#include <fstream>
#include <iterator>
#include <format>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace MPL::SliderStorage
{
    namespace
    {
        std::optional<std::string> Read(const std::filesystem::path& a_path)
        {
            std::ifstream stream(a_path, std::ios::binary);
            if (!stream) return std::nullopt;
            return std::string(std::istreambuf_iterator<char>(stream), {});
        }
        bool Write(const std::filesystem::path& a_path, const std::string_view a_text)
        {
            std::ofstream stream(a_path, std::ios::binary | std::ios::trunc);
            stream << a_text;
            stream.flush();
            return stream.good();
        }
        bool Replace(const std::filesystem::path& a_source, const std::filesystem::path& a_target)
        {
            return ::MoveFileExW(a_source.c_str(), a_target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        }
        struct Change
        {
            std::filesystem::path path;
            std::string original;
            std::string updated;
            std::filesystem::path staged;
        };
    }

    bool PrepareDraft(const std::filesystem::path& a_path, std::string& a_error)
    {
        const auto text = Read(a_path);
        const auto draft = text ? DraftLayout(*text, a_error) : std::nullopt;
        if (!draft) return false;
        auto staged = a_path;
        staged += ".tmp";
        if (Write(staged, *draft) && Replace(staged, a_path)) return true;
        a_error = "The draft slider identities could not be saved.";
        return false;
    }

    bool CommitLayout(const std::filesystem::path& a_path, const std::string_view a_layout,
        const std::filesystem::path& a_userPath, std::string& a_error, const std::optional<std::size_t> a_page,
        const std::optional<DraftUpdate>& a_draft)
    {
        a_error.clear();
        const auto original = Read(a_path);
        const auto canonical = CanonicalLayout(a_layout, a_error, a_page);
        if (!original || !canonical) return false;
        const auto oldBindings = ReadLayout(*original, a_error);
        const auto newBindings = ReadLayout(a_layout, a_error);
        if (!a_error.empty()) return false;
        std::vector<Change> changes;
        const auto add = [&](const std::filesystem::path& path, const bool presets)
        {
            if (path.empty()) return true;
            std::error_code error;
            if (!std::filesystem::exists(path, error)) return !error;
            const auto text = Read(path);
            const auto updated = text ? (presets ? RemapPresets(*text, oldBindings, newBindings, a_error) :
                Remap(*text, oldBindings, newBindings, a_error)) : std::nullopt;
            if (!updated) return false;
            if (*updated != *text) changes.push_back({ path, *text, *updated, {} });
            return true;
        };
        if (!add(a_path.parent_path() / "profileSettings.json", false) ||
            !add(a_path.parent_path() / "presets.json", true) || !add(a_userPath, false))
        {
            if (a_error.empty()) a_error = "Slider values or presets could not be prepared. No files were changed.";
            return false;
        }
        changes.push_back({ a_path, *original, *canonical, {} });
        if (a_draft)
        {
            const auto draftOriginal = Read(a_draft->path);
            const auto draft = DraftLayout(a_draft->text, a_error);
            if (!draftOriginal || !draft || a_draft->path == a_path)
            {
                if (a_error.empty()) a_error = "The working layout could not be prepared. No files were changed.";
                return false;
            }
            changes.push_back({ a_draft->path, *draftOriginal, *draft, {} });
        }
        std::error_code error;
        const auto cleanup = [&]()
        {
            for (const auto& change : changes)
                if (!change.staged.empty()) std::filesystem::remove(change.staged, error);
        };
        for (auto& change : changes)
        {
            change.staged = change.path;
            change.staged += ".slider-values.tmp";
            auto backup = change.path;
            backup += ".before-slider-values";
            if (!Write(change.staged, change.updated) ||
                (!std::filesystem::exists(backup) && !Write(backup, change.original)))
            {
                cleanup();
                a_error = "The slider migration could not be staged or backed up. Existing files were not changed.";
                return false;
            }
        }
        for (std::size_t index = 0; index < changes.size(); ++index)
        {
            if (Replace(changes[index].staged, changes[index].path)) continue;
            bool restored = true;
            for (std::size_t previous = 0; previous < index; ++previous)
                restored &= Write(changes[previous].staged, changes[previous].original) &&
                    Replace(changes[previous].staged, changes[previous].path);
            cleanup();
            a_error = restored ? "The slider migration could not be committed. Existing files were restored." :
                "The slider migration failed during rollback. Recover the preserved .before-slider-values backups.";
            return false;
        }
        return true;
    }
}
