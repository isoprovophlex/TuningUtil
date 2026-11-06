#pragma once

#include <Config/Weathers.h>
#include <SettingLinks.h>
#include <CompressionMath.h>
#include <HueMath.h>
#include <RE/B/BGSDirectionalAmbientLightingColors.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace MPL::WeatherPatcher
{
    inline std::uint8_t ClampByte(double a_value)
    {
        return static_cast<std::uint8_t>(std::clamp(std::round(a_value), 0.0, 255.0));
    }

    inline double Luminance(const RE::Color& a_color)
    {
        return 0.299 * a_color.red + 0.587 * a_color.green + 0.114 * a_color.blue;
    }

    inline double HSVValue(const RE::Color& a_color)
    {
        return std::max({
            static_cast<double>(a_color.red),
            static_cast<double>(a_color.green),
            static_cast<double>(a_color.blue),
        });
    }

    inline bool IsBlack(const RE::Color& a_color)
    {
        return a_color.red == 0 && a_color.green == 0 && a_color.blue == 0;
    }

    inline void NormalizeLuminanceColor(RE::Color& a_color, const double a_target)
    {
        const auto luminance = Luminance(a_color);
        if (luminance <= 0.0 || !std::isfinite(a_target) || a_target < 0.0 || a_target > 255.0) return;
        const auto maxChannel = HSVValue(a_color);
        const auto gain = std::min(a_target / luminance, 255.0 / maxChannel);
        a_color.red = ClampByte(a_color.red * gain);
        a_color.green = ClampByte(a_color.green * gain);
        a_color.blue = ClampByte(a_color.blue * gain);
    }

    inline double DirectionalAmbientLuminance(const RE::BGSDirectionalAmbientLightingColors& a_colors)
    {
        constexpr double zMinusWeight = 0.50;
        constexpr double horizontalWeight = 0.30;
        constexpr double zPlusWeight = 0.20;
        const auto& directions = a_colors.directional;
        const auto horizontal = (Luminance(directions.x.max) + Luminance(directions.x.min) +
            Luminance(directions.y.max) + Luminance(directions.y.min)) / 4.0;
        return zMinusWeight * Luminance(directions.z.min) + horizontalWeight * horizontal +
            zPlusWeight * Luminance(directions.z.max);
    }

    inline void NormalizeDirectionalAmbientLuminance(
        RE::BGSDirectionalAmbientLightingColors& a_colors, const double a_target)
    {
        const auto luminance = DirectionalAmbientLuminance(a_colors);
        if (luminance <= 0.0 || !std::isfinite(a_target) || a_target < 0.0 || a_target > 255.0) return;
        const std::array colors{ &a_colors.directional.x.max, &a_colors.directional.x.min,
            &a_colors.directional.y.max, &a_colors.directional.y.min,
            &a_colors.directional.z.max, &a_colors.directional.z.min };
        double maxChannel = 0.0;
        for (const auto* color : colors) maxChannel = std::max(maxChannel, HSVValue(*color));
        const auto gain = std::min(a_target / luminance, 255.0 / maxChannel);
        for (auto* color : colors)
        {
            color->red = ClampByte(color->red * gain);
            color->green = ClampByte(color->green * gain);
            color->blue = ClampByte(color->blue * gain);
        }
    }

    inline double GainHSVValue(const RE::Color& a_color)
    {
        return IsBlack(a_color) ? 1.0 : HSVValue(a_color);
    }

    inline void MultiplyColor(RE::Color& a_color, double a_multiplier)
    {
        double multiplier = std::max(0.0, a_multiplier);
        if (multiplier > 1.0)
        {
            const double maxChannel = std::max({ static_cast<double>(a_color.red), static_cast<double>(a_color.green), static_cast<double>(a_color.blue) });
            if (maxChannel > 0.0)
            {
                multiplier = std::min(multiplier, 255.0 / maxChannel);
            }
        }
        else if (multiplier < 1.0 && !IsBlack(a_color))
        {
            double minPositiveChannel = 255.0;
            for (const auto channel : { a_color.red, a_color.green, a_color.blue })
            {
                if (channel > 0)
                {
                    minPositiveChannel = std::min(minPositiveChannel, static_cast<double>(channel));
                }
            }
            multiplier = std::max(multiplier, 1.0 / minPositiveChannel);
        }

        a_color.red = ClampByte(a_color.red * multiplier);
        a_color.green = ClampByte(a_color.green * multiplier);
        a_color.blue = ClampByte(a_color.blue * multiplier);
    }

    inline void MultiplyBrightnessColor(RE::Color& a_color, double a_multiplier)
    {
        double multiplier = std::max(0.1, a_multiplier);
        if (multiplier > 1.0)
        {
            const double value = HSVValue(a_color);
            if (value > 0.0)
            {
                multiplier = std::min(multiplier, 255.0 / value);
            }
        }

        a_color.red = ClampByte(a_color.red * multiplier);
        a_color.green = ClampByte(a_color.green * multiplier);
        a_color.blue = ClampByte(a_color.blue * multiplier);
    }

    inline void MultiplyBrightnessColor(RE::NiColor& a_color, double a_multiplier)
    {
        double multiplier = std::max(0.1, a_multiplier);
        if (multiplier > 1.0)
        {
            const double value = std::max({ a_color.red, a_color.green, a_color.blue });
            if (value > 0.0)
            {
                multiplier = std::min(multiplier, 1.0 / value);
            }
        }

        a_color.red = static_cast<float>(std::clamp(a_color.red * multiplier, 0.0, 1.0));
        a_color.green = static_cast<float>(std::clamp(a_color.green * multiplier, 0.0, 1.0));
        a_color.blue = static_cast<float>(std::clamp(a_color.blue * multiplier, 0.0, 1.0));
    }

    inline double CompressedBrightnessValue(
        const double a_value,
        const double a_compress,
        const double a_anchor)
    {
        if (a_value <= 0.0001 || a_anchor <= 0.0001)
        {
            return 0.0;
        }
        return std::exp(
            (1.0 - a_compress) * std::log(a_value) +
            a_compress * std::log(a_anchor));
    }

    inline void CompressColor(RE::Color& a_color, double a_compress, double a_anchor)
    {
        const double inputRed = IsBlack(a_color) ? 1.0 : a_color.red;
        const double inputGreen = IsBlack(a_color) ? 1.0 : a_color.green;
        const double inputBlue = IsBlack(a_color) ? 1.0 : a_color.blue;
        const double value = std::max({ inputRed, inputGreen, inputBlue });
        const double compressedValue = CompressedBrightnessValue(value, a_compress, a_anchor);

        double red = compressedValue;
        double green = compressedValue;
        double blue = compressedValue;

        if (value > 0.0001)
        {
            double scale = compressedValue / value;
            if (scale > 1.0)
            {
                const double maxInputChannel = std::max({ inputRed, inputGreen, inputBlue });
                scale = std::min(scale, 255.0 / maxInputChannel);
            }
            else if (scale < 1.0)
            {
                double minPositiveInputChannel = 255.0;
                for (const auto channel : { inputRed, inputGreen, inputBlue })
                {
                    if (channel > 0.0)
                    {
                        minPositiveInputChannel = std::min(minPositiveInputChannel, channel);
                    }
                }
                scale = std::max(scale, 1.0 / minPositiveInputChannel);
            }
            red = inputRed * scale;
            green = inputGreen * scale;
            blue = inputBlue * scale;
        }

        const double maxChannel = std::max({ red, green, blue });
        if (maxChannel > 255.0)
        {
            const double scale = 255.0 / maxChannel;
            red *= scale;
            green *= scale;
            blue *= scale;
        }

        a_color.red = ClampByte(red);
        a_color.green = ClampByte(green);
        a_color.blue = ClampByte(blue);
    }

    inline void ApplyCompressionGain(RE::Color& a_color, double a_gain)
    {
        if (IsBlack(a_color) && std::abs(a_gain - 1.0) > 0.0001)
        {
            a_color.red = 1;
            a_color.green = 1;
            a_color.blue = 1;
        }
        MultiplyColor(a_color, a_gain);
    }

    inline void SaturateColor(RE::Color& a_color, double a_multiplier)
    {
        const double factor = std::max(0.0, a_multiplier);
        const double luminance = Luminance(a_color);
        const double red = luminance + ((a_color.red - luminance) * factor);
        const double green = luminance + ((a_color.green - luminance) * factor);
        const double blue = luminance + ((a_color.blue - luminance) * factor);

        a_color.red = ClampByte(red);
        a_color.green = ClampByte(green);
        a_color.blue = ClampByte(blue);
    }

    inline void SaturateColor(RE::NiColor& a_color, double a_multiplier)
    {
        const double factor = std::max(0.0, a_multiplier);
        const double luminance = 0.299 * a_color.red + 0.587 * a_color.green + 0.114 * a_color.blue;
        a_color.red = static_cast<float>(std::clamp(luminance + ((a_color.red - luminance) * factor), 0.0, 1.0));
        a_color.green = static_cast<float>(std::clamp(luminance + ((a_color.green - luminance) * factor), 0.0, 1.0));
        a_color.blue = static_cast<float>(std::clamp(luminance + ((a_color.blue - luminance) * factor), 0.0, 1.0));
    }

    inline std::optional<double> HueRangeValue(const double a_red, const double a_green, const double a_blue)
    {
        return HueMath::Value(a_red, a_green, a_blue);
    }

    inline std::optional<double> HueRangeValue(const RE::Color& a_color)
    {
        return HueRangeValue(a_color.red / 255.0, a_color.green / 255.0, a_color.blue / 255.0);
    }

    inline std::optional<double> HueRangeValue(const RE::NiColor& a_color)
    {
        return HueRangeValue(a_color.red, a_color.green, a_color.blue);
    }

    inline double NormalizeHueRangeValue(const double a_hue)
    {
        return HueMath::Normalize(a_hue);
    }

    inline double HueRangeWeight(const double a_hue, const HueRange& a_range)
    {
        if (std::abs(a_range.end - a_range.start) >= 254.999)
        {
            return 1.0;
        }
        const double start = NormalizeHueRangeValue(a_range.start);
        const double span = NormalizeHueRangeValue(a_range.end - a_range.start);
        if (span <= 0.0001)
        {
            return 0.0;
        }
        const double offset = NormalizeHueRangeValue(a_hue - start);
        if (offset > span)
        {
            return 0.0;
        }
        const double halfSpan = span * 0.5;
        return std::max(0.0, 1.0 - std::abs(offset - halfSpan) / halfSpan);
    }

    template <class Color>
    inline std::optional<std::array<double, 7>> HueBandWeights(const Color& a_color, const HueRanges& a_ranges)
    {
        const auto hue = HueRangeValue(a_color);
        if (!hue)
        {
            return std::nullopt;
        }
        const std::array<const HueRange*, 7> ranges{
            &a_ranges.red,
            &a_ranges.orange,
            &a_ranges.yellow,
            &a_ranges.green,
            &a_ranges.teal,
            &a_ranges.blue,
            &a_ranges.magenta,
        };
        std::array<double, 7> weights{};
        for (std::size_t i = 0; i < ranges.size(); ++i)
        {
            weights[i] = HueRangeWeight(*hue, *ranges[i]);
        }
        return weights;
    }

    inline double HueBandValue(const AmbientHueScaleValues& a_values, const std::size_t a_index)
    {
        const std::array values{
            a_values.red, a_values.orange, a_values.yellow, a_values.green,
            a_values.teal, a_values.blue, a_values.magenta
        };
        return values[a_index];
    }

    template <class Color>
    inline double HueScale(const Color& a_color, const AmbientHueScaleValues& a_scales, const HueRanges& a_ranges)
    {
        const auto weights = HueBandWeights(a_color, a_ranges);
        if (!weights)
        {
            return 1.0;
        }
        double totalWeight = 0.0;
        double scale = 0.0;
        for (std::size_t i = 0; i < weights->size(); ++i)
        {
            totalWeight += (*weights)[i];
            scale += HueBandValue(a_scales, i) * (*weights)[i];
        }
        return totalWeight > 0.0001 ? std::max(0.0, scale / totalWeight) : 1.0;
    }

    inline double ColorHueScale(
        const RE::Color& a_color,
        const AmbientHueScaleValues& a_scales,
        const HueRanges& a_ranges)
    {
        return HueScale(a_color, a_scales, a_ranges);
    }

    inline double ColorHueScale(
        const RE::NiColor& a_color,
        const AmbientHueScaleValues& a_scales,
        const HueRanges& a_ranges)
    {
        return HueScale(a_color, a_scales, a_ranges);
    }

    inline std::array<double, 3> HsvColor(const double a_hue, const double a_saturation)
    {
        const double hue = std::fmod(std::fmod(a_hue, 360.0) + 360.0, 360.0) / 60.0;
        const double chroma = std::clamp(a_saturation, 0.0, 1.0);
        const double secondary = chroma * (1.0 - std::abs(std::fmod(hue, 2.0) - 1.0));
        std::array<double, 3> rgb{};
        switch (static_cast<std::uint32_t>(hue))
        {
        case 0: rgb = { chroma, secondary, 0.0 }; break;
        case 1: rgb = { secondary, chroma, 0.0 }; break;
        case 2: rgb = { 0.0, chroma, secondary }; break;
        case 3: rgb = { 0.0, secondary, chroma }; break;
        case 4: rgb = { secondary, 0.0, chroma }; break;
        default: rgb = { chroma, 0.0, secondary }; break;
        }
        const double minimum = 1.0 - chroma;
        for (auto& channel : rgb) channel += minimum;
        return rgb;
    }

    inline double NormalizedLuminance(const std::array<double, 3>& a_rgb)
    {
        return 0.299 * a_rgb[0] + 0.587 * a_rgb[1] + 0.114 * a_rgb[2];
    }

    inline std::array<double, 3> RotateHuePreservingSaturationAndLuminance(
        const double a_red,
        const double a_green,
        const double a_blue,
        const double a_degrees)
    {
        const std::array source{
            std::clamp(a_red, 0.0, 1.0),
            std::clamp(a_green, 0.0, 1.0),
            std::clamp(a_blue, 0.0, 1.0),
        };
        const double maximum = std::ranges::max(source);
        const double minimum = std::ranges::min(source);
        const double saturation = maximum > 0.000001 ? (maximum - minimum) / maximum : 0.0;
        const auto sourceHue = HueRangeValue(source[0], source[1], source[2]);
        if (!sourceHue || saturation <= 0.000001)
        {
            return source;
        }

        const double hue = *sourceHue * (360.0 / 255.0);
        const double luminance = NormalizedLuminance(source);
        auto rgb = HsvColor(hue + a_degrees, saturation);
        const double unitLuminance = NormalizedLuminance(rgb);
        const double value = unitLuminance > 0.000001 ? std::min(1.0, luminance / unitLuminance) : 0.0;
        for (auto& channel : rgb) channel = std::clamp(channel * value, 0.0, 1.0);
        return rgb;
    }

    inline void ShiftHue(RE::Color& a_color, const double a_degrees)
    {
        if (std::abs(a_degrees) <= 0.0001)
        {
            return;
        }
        const auto rgb = RotateHuePreservingSaturationAndLuminance(
            a_color.red / 255.0,
            a_color.green / 255.0,
            a_color.blue / 255.0,
            a_degrees);
        a_color.red = ClampByte(rgb[0] * 255.0);
        a_color.green = ClampByte(rgb[1] * 255.0);
        a_color.blue = ClampByte(rgb[2] * 255.0);
    }

    inline void ShiftHue(RE::NiColor& a_color, const double a_degrees)
    {
        if (std::abs(a_degrees) <= 0.0001)
        {
            return;
        }
        const auto rgb = RotateHuePreservingSaturationAndLuminance(a_color.red, a_color.green, a_color.blue, a_degrees);
        a_color.red = static_cast<float>(rgb[0]);
        a_color.green = static_cast<float>(rgb[1]);
        a_color.blue = static_cast<float>(rgb[2]);
    }

    inline double ColorHueShiftDegrees(
        const RE::Color& a_color,
        const HueShiftBands& a_shift,
        const HueRanges& a_ranges)
    {
        const auto weights = HueBandWeights(a_color, a_ranges);
        if (!weights)
        {
            return 0.0;
        }
        const AmbientHueScaleValues shifts{
            a_shift.red,
            a_shift.orange,
            a_shift.yellow,
            a_shift.green,
            a_shift.teal,
            a_shift.blue,
            a_shift.magenta,
        };
        double totalWeight = 0.0;
        double degrees = 0.0;
        for (std::size_t i = 0; i < weights->size(); ++i)
        {
            totalWeight += (*weights)[i];
            degrees += HueBandValue(shifts, i) * (*weights)[i];
        }
        return totalWeight > 0.0001 ? degrees / totalWeight : 0.0;
    }

    inline double ColorHueShiftDegrees(
        const RE::NiColor& a_color,
        const HueShiftBands& a_shift,
        const HueRanges& a_ranges)
    {
        const auto weights = HueBandWeights(a_color, a_ranges);
        if (!weights)
        {
            return 0.0;
        }
        const AmbientHueScaleValues shifts{
            a_shift.red,
            a_shift.orange,
            a_shift.yellow,
            a_shift.green,
            a_shift.teal,
            a_shift.blue,
            a_shift.magenta,
        };
        double totalWeight = 0.0;
        double degrees = 0.0;
        for (std::size_t i = 0; i < weights->size(); ++i)
        {
            totalWeight += (*weights)[i];
            degrees += HueBandValue(shifts, i) * (*weights)[i];
        }
        return totalWeight > 0.0001 ? degrees / totalWeight : 0.0;
    }

}
