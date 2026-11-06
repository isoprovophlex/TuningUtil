#pragma once

#include <chrono>
#include <optional>

namespace MPL::MenuEffects
{
    class RecoveryWindow
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr auto Duration = std::chrono::milliseconds{ 500 };

        void Schedule()
        {
            pending = true;
            deadline.reset();
        }

        void Cancel()
        {
            pending = false;
            deadline.reset();
        }

        bool IsPending() const { return pending; }

        bool ShouldRecover(const Clock::time_point a_now, const bool a_frameworkOpen,
            const bool a_blockingWindowOpen, const bool a_gameMenuActive)
        {
            if (a_frameworkOpen) Cancel();
            if (!pending) return false;
            if (!deadline) deadline = a_now + Duration;
            if (a_now >= *deadline)
            {
                Cancel();
                return false;
            }
            return !a_blockingWindowOpen && !a_gameMenuActive;
        }

    private:
        bool pending = false;
        std::optional<Clock::time_point> deadline;
    };

    inline bool ClearOrphanedBlur(auto& a_blur)
    {
        if (a_blur.blurCount == 0) return false;
        // Collapse orphaned contributions before asking the engine to release the effect.
        a_blur.blurCount = 1;
        a_blur.DecrementBlurCount();
        return true;
    }
}
