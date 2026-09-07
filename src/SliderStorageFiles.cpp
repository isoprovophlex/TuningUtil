#include <SliderStorageFiles.h>
#include <SliderStorage.h>
#include <FileIO.h>
#include <chrono>
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
            std::string result(std::istreambuf_iterator<char>(stream), {});
            return stream.bad() ? std::nullopt : std::optional{ std::move(result) };
        }
        struct Change
        {
            std::filesystem::path path;
            std::string original;
            std::string updated;
            std::filesystem::path staged;
            std::filesystem::path recovery;
        };
    }

    bool PrepareDraft(const std::filesystem::path& a_path, std::string& a_error)
    {
        const auto text = Read(a_path);
        const auto draft = text ? DraftLayout(*text, a_error) : std::nullopt;
        if (!draft) return false;
        return FileIO::WriteAtomically(a_path, *draft, a_error);
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
        if (!a_error.empty()) return false;
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
            {
                if (!change.staged.empty()) std::filesystem::remove(change.staged, error);
                if (!change.recovery.empty()) std::filesystem::remove(change.recovery, error);
            }
        };
        const auto recoverySuffix = std::format(".before-layout-commit-{}-{}", ::GetCurrentProcessId(),
            std::chrono::steady_clock::now().time_since_epoch().count());
        for (auto& change : changes)
        {
            change.staged = change.path;
            change.staged += ".slider-values.tmp";
            change.recovery = change.path;
            change.recovery += recoverySuffix;
            auto backup = change.path;
            backup += ".before-slider-values";
            const auto hasBackup = std::filesystem::exists(backup, error);
            if (error || !FileIO::WriteClosed(change.staged, change.updated, a_error) ||
                !FileIO::WriteClosed(change.recovery, change.original, a_error) ||
                (!hasBackup && !FileIO::WriteAtomically(backup, change.original, a_error)))
            {
                cleanup();
                a_error = "The slider migration could not be staged or backed up. Existing files were not changed.";
                return false;
            }
        }
        for (std::size_t index = 0; index < changes.size(); ++index)
        {
            if (FileIO::Replace(changes[index].staged, changes[index].path, a_error)) continue;
            bool restored = true;
            for (std::size_t previous = 0; previous < index; ++previous)
                restored &= FileIO::WriteClosed(changes[previous].staged, changes[previous].original, a_error) &&
                    FileIO::Replace(changes[previous].staged, changes[previous].path, a_error);
            if (restored) cleanup();
            a_error = restored ? "The slider migration could not be committed. Existing files were restored." :
                "The slider migration failed during rollback. Recover the preserved " + recoverySuffix + " backups.";
            return false;
        }
        cleanup();
        return true;
    }
}
