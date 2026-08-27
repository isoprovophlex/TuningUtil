#include <CSTonemapping.h>

#include <cmath>
#include <mutex>
#include <ranges>
#include <unordered_map>

namespace MPL::CSTonemapping
{
    namespace
    {
        constexpr auto kFilmicCurveSetting =
            "bUseFilmicCurve:Display";
        constexpr auto kFilmicWhiteScaleSetting =
            "fFilmicWhiteScale:Display";
        constexpr float kWhitePoint = 0.1f;
        constexpr float kWhiteScale = 10.0f;

        struct State
        {
            std::mutex lock;
            std::unordered_map<RE::TESImageSpace*, float>
                whiteBaselines;
            std::optional<bool> filmicCurveBaseline;
            std::optional<float> filmicWhiteScaleBaseline;
            std::unordered_set<RE::TESImageSpace*> forcedTargets;
            std::unordered_set<RE::TESImageSpace*>
                appliedForcedTargets;
            bool initialized = false;
        };

        State& GetState()
        {
            static State state;
            return state;
        }

        bool IsWhitePoint(const float a_value)
        {
            return std::abs(a_value - kWhitePoint) <= 0.0001f;
        }

        void CaptureBaselines(
            State& a_state,
            RE::TESDataHandler* a_dataHandler)
        {
            if (auto* setting =
                    RE::GetINISetting(kFilmicCurveSetting);
                setting && !a_state.filmicCurveBaseline)
            {
                a_state.filmicCurveBaseline =
                    setting->GetBool();
            }
            if (auto* setting =
                    RE::GetINISetting(kFilmicWhiteScaleSetting);
                setting && !a_state.filmicWhiteScaleBaseline)
            {
                a_state.filmicWhiteScaleBaseline =
                    setting->GetFloat();
            }
            for (auto* imageSpace :
                 a_dataHandler->GetFormArray<RE::TESImageSpace>())
            {
                if (imageSpace)
                {
                    a_state.whiteBaselines.try_emplace(
                        imageSpace,
                        imageSpace->data.hdr.white);
                }
            }
        }

        void ApplyLocked(State& a_state)
        {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler)
            {
                return;
            }
            CaptureBaselines(a_state, dataHandler);

            for (auto* imageSpace :
                 a_state.appliedForcedTargets)
            {
                if (a_state.forcedTargets.contains(imageSpace))
                {
                    continue;
                }
                const auto baseline =
                    a_state.whiteBaselines.find(imageSpace);
                if (baseline != a_state.whiteBaselines.end())
                {
                    imageSpace->data.hdr.white =
                        baseline->second;
                }
            }
            for (auto* imageSpace : a_state.forcedTargets)
            {
                if (imageSpace)
                {
                    imageSpace->data.hdr.white = kWhitePoint;
                }
            }
            a_state.appliedForcedTargets =
                a_state.forcedTargets;

            const auto forceFilmic =
                !a_state.forcedTargets.empty() ||
                std::ranges::any_of(
                    dataHandler
                        ->GetFormArray<RE::TESImageSpace>(),
                    [](const RE::TESImageSpace* a_imageSpace)
                    {
                        return a_imageSpace &&
                               IsWhitePoint(
                                   a_imageSpace->data.hdr.white);
                    });
            if (auto* setting =
                    RE::GetINISetting(kFilmicCurveSetting))
            {
                setting->SetBool(
                    forceFilmic ?
                        true :
                        a_state.filmicCurveBaseline.value_or(
                            setting->GetBool()));
            }
            if (auto* setting =
                    RE::GetINISetting(kFilmicWhiteScaleSetting))
            {
                setting->SetFloat(
                    forceFilmic ?
                        kWhiteScale :
                        a_state.filmicWhiteScaleBaseline.value_or(
                            setting->GetFloat()));
            }
            logger::info(
                "CS Tonemapping coordinated {} forced Image "
                "Space target(s); filmic settings forced: {}",
                a_state.forcedTargets.size(),
                forceFilmic);
        }
    }

    void Initialize()
    {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn(
                "TESDataHandler is unavailable; CS Tonemapping "
                "was not initialized");
            return;
        }

        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        CaptureBaselines(state, dataHandler);
        state.initialized = true;
        ApplyLocked(state);
    }

    void SetForcedTargets(
        const std::unordered_set<RE::TESImageSpace*>& a_targets)
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.forcedTargets = a_targets;
        if (state.initialized)
        {
            ApplyLocked(state);
        }
    }

    void ReleaseRuntimeState()
    {
        auto& state = GetState();
        std::scoped_lock lock(state.lock);
        state.whiteBaselines.clear();
        state.filmicCurveBaseline.reset();
        state.filmicWhiteScaleBaseline.reset();
        state.forcedTargets.clear();
        state.appliedForcedTargets.clear();
        state.initialized = false;
    }
}
