#pragma once

#include <array>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace MPL::SKSEMenuSettings
{
    enum class StatusLocation
    {
        top,
        bottom,
        hidden,
    };

    enum class ButtonKind
    {
        ordinary,
        save,
        restore,
        reset,
        destructive,
    };

    enum class PresetState
    {
        active,
        modified,
    };

    enum class InteractionState
    {
        normal,
        hovered,
        pressed,
    };

    enum class TooltipDelay
    {
        none,
        shortDelay,
        normal,
    };

    using Color = std::array<float, 4>;

    struct SliderDefaults
    {
        float minimum = std::numeric_limits<float>::quiet_NaN();
        float maximum = std::numeric_limits<float>::quiet_NaN();
        float width = 0.0f;
        float step = 0.0f;
        std::string format = "%.2f";
    };

    using MessageArgument = std::pair<std::string_view, std::string>;

    void NormalizeJsonColor(Color& a_color);
    void Initialize();
    void ReloadIfChanged();

    StatusLocation GetStatusLocation();
    std::chrono::duration<double> GetStatusDuration();
    float GetStatusFontScale();
    float GetStatusHeight();
    std::optional<Color> GetStatusColor();
    bool ClearStatusOnPageChange();

    float GetHoverBrightness();
    float GetPressedBrightness();
    std::optional<Color> GetButtonColor(ButtonKind a_kind);
    Color GetPresetColor(PresetState a_state, InteractionState a_interaction);

    std::string Label(std::string_view a_key, std::string_view a_fallback);
    std::string SettingGroupLabel(std::string_view a_key, std::string_view a_fallback);
    std::string SettingNameLabel(std::string_view a_key, std::string_view a_fallback);
    std::string SettingHueLabel(std::string_view a_key, std::string_view a_fallback);
    std::string SettingPathLabel(std::string_view a_path, std::string_view a_fallback);
    std::string SettingLabelSeparator();

    float GetActionButtonSpacing();
    float GetPageTopSpacing();
    float GetSectionSpacing();
    float GetHeaderFontScale();
    std::array<float, 2> GetBoxPadding();
    SliderDefaults ResolveSliderDefaults(
        std::string_view a_setting,
        float a_fallbackWidth,
        float a_fallbackStep,
        std::string_view a_fallbackFormat,
        float a_fallbackMinimum = std::numeric_limits<float>::quiet_NaN(),
        float a_fallbackMaximum = std::numeric_limits<float>::quiet_NaN());

    bool SettingsProfileFirst();
    std::string SettingsProfileLabel(bool a_includeGap);

    TooltipDelay GetTooltipDelay();
    float GetTooltipFontScale();
    std::optional<Color> GetTooltipTextColor();
    std::optional<Color> GetTooltipBackgroundColor();

    std::string StatusMessage(
        std::string_view a_key,
        std::initializer_list<MessageArgument> a_arguments = {});
    std::string DisplayMessage(
        std::string_view a_key,
        std::initializer_list<MessageArgument> a_arguments = {});
}  // namespace MPL::SKSEMenuSettings
