#pragma once

#include <unordered_set>

namespace RE
{
    class TESImageSpace;
}

namespace MPL::CSTonemapping
{
    void Initialize();
    void SetForcedTargets(const std::unordered_set<RE::TESImageSpace*>&);
    void ReleaseRuntimeState();
}
