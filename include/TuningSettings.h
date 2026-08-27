#pragma once

namespace MPL::TuningSettings
{
    void Load();
    bool IsTuningMenuEnabledForSession();
    bool IsTuningMenuConfigured();
    bool IsDetailedLoggingConfigured();
    bool SetTuningMenuConfigured(bool);
    bool SetDetailedLoggingConfigured(bool);
}  // namespace MPL::TuningSettings
