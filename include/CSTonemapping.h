#pragma once

#include <unordered_set>

namespace RE
{
    class TESImageSpace;
}

namespace MPL::HeliosphanAPI
{
    struct Interface;
}

namespace MPL::CSTonemapping
{
    const HeliosphanAPI::Interface* GetHeliosphanAPI();
    void Initialize();
    void SetForcedTargets(const std::unordered_set<RE::TESImageSpace*>&);
    void ReleaseRuntimeState();
}
