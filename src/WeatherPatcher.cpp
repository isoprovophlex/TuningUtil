#include <Config.h>
#include <Config/Forms.h>
#include <DetailedLogging.h>
#include <JsonOverlay.h>
#include <RecordFilter.h>
#include <TuningSettings.h>
#include <SettingLinks.h>
#include <WeatherPatcher.h>
#include <WeatherRuntime.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace MPL::WeatherPatcher
{
    namespace
    {
        bool emittanceWeatherSettingsWereApplied = false;
    }

    std::uint8_t ClampByte(double a_value)
    {
        return static_cast<std::uint8_t>(std::clamp(std::round(a_value), 0.0, 255.0));
    }

    double Luminance(const RE::Color& a_color)
    {
        return 0.299 * a_color.red + 0.587 * a_color.green + 0.114 * a_color.blue;
    }

    double HSVValue(const RE::Color& a_color)
    {
        return std::max({
            static_cast<double>(a_color.red),
            static_cast<double>(a_color.green),
            static_cast<double>(a_color.blue),
        });
    }

    bool IsBlack(const RE::Color& a_color)
    {
        return a_color.red == 0 && a_color.green == 0 && a_color.blue == 0;
    }

    double GainHSVValue(const RE::Color& a_color)
    {
        return IsBlack(a_color) ? 1.0 : HSVValue(a_color);
    }

    void MultiplyColor(RE::Color& a_color, double a_multiplier)
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

    void MultiplyBrightnessColor(RE::Color& a_color, double a_multiplier)
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

    void CompressColor(RE::Color& a_color, double a_compress, double a_anchor)
    {
        const double inputRed = IsBlack(a_color) ? 1.0 : a_color.red;
        const double inputGreen = IsBlack(a_color) ? 1.0 : a_color.green;
        const double inputBlue = IsBlack(a_color) ? 1.0 : a_color.blue;
        const double value = std::max({ inputRed, inputGreen, inputBlue });
        double compressedValue = 0.0;

        if (value > 0.0001 && a_anchor > 0.0001)
        {
            compressedValue = std::exp((1.0 - a_compress) * std::log(value) + a_compress * std::log(a_anchor));
        }

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

    void ApplyCompressionGain(RE::Color& a_color, double a_gain)
    {
        if (IsBlack(a_color) && std::abs(a_gain - 1.0) > 0.0001)
        {
            a_color.red = 1;
            a_color.green = 1;
            a_color.blue = 1;
        }
        MultiplyColor(a_color, a_gain);
    }

    void SaturateColor(RE::Color& a_color, double a_multiplier)
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

    void SaturateColor(RE::NiColor& a_color, double a_multiplier)
    {
        const double factor = std::max(0.0, a_multiplier);
        const double luminance = 0.299 * a_color.red + 0.587 * a_color.green + 0.114 * a_color.blue;
        a_color.red = static_cast<float>(std::clamp(luminance + ((a_color.red - luminance) * factor), 0.0, 1.0));
        a_color.green = static_cast<float>(std::clamp(luminance + ((a_color.green - luminance) * factor), 0.0, 1.0));
        a_color.blue = static_cast<float>(std::clamp(luminance + ((a_color.blue - luminance) * factor), 0.0, 1.0));
    }

    std::optional<double> HueRangeValue(const double a_red, const double a_green, const double a_blue)
    {
        const double maximum = std::max({ a_red, a_green, a_blue });
        const double minimum = std::min({ a_red, a_green, a_blue });
        const double delta = maximum - minimum;
        if (delta <= 0.0001)
        {
            return std::nullopt;
        }

        double hue = 0.0;
        if (maximum == a_red)
        {
            hue = 60.0 * std::fmod((a_green - a_blue) / delta, 6.0);
        }
        else if (maximum == a_green)
        {
            hue = 60.0 * (((a_blue - a_red) / delta) + 2.0);
        }
        else
        {
            hue = 60.0 * (((a_red - a_green) / delta) + 4.0);
        }
        const auto degrees = hue < 0.0 ? hue + 360.0 : hue;
        return degrees * (255.0 / 360.0);
    }

    std::optional<double> HueRangeValue(const RE::Color& a_color)
    {
        return HueRangeValue(a_color.red / 255.0, a_color.green / 255.0, a_color.blue / 255.0);
    }

    std::optional<double> HueRangeValue(const RE::NiColor& a_color)
    {
        return HueRangeValue(a_color.red, a_color.green, a_color.blue);
    }

    double NormalizeHueRangeValue(const double a_hue)
    {
        const double normalized = std::fmod(a_hue, 255.0);
        return normalized < 0.0 ? normalized + 255.0 : normalized;
    }

    double HueRangeWeight(const double a_hue, const HueRange& a_range)
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
    std::optional<std::array<double, 7>> HueBandWeights(const Color& a_color, const HueRanges& a_ranges)
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

    double HueBandValue(const AmbientHueScaleValues& a_values, const std::size_t a_index)
    {
        const std::array values{
            a_values.red, a_values.orange, a_values.yellow, a_values.green,
            a_values.teal, a_values.blue, a_values.magenta
        };
        return values[a_index];
    }

    template <class Color>
    double HueScale(const Color& a_color, const AmbientHueScaleValues& a_scales, const HueRanges& a_ranges)
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

    double ColorHueScale(
        const RE::Color& a_color,
        const AmbientHueScaleValues& a_scales,
        const HueRanges& a_ranges)
    {
        return HueScale(a_color, a_scales, a_ranges);
    }

    double ColorHueScale(
        const RE::NiColor& a_color,
        const AmbientHueScaleValues& a_scales,
        const HueRanges& a_ranges)
    {
        return HueScale(a_color, a_scales, a_ranges);
    }

    std::array<double, 3> HsvColor(const double a_hue, const double a_saturation)
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

    double NormalizedLuminance(const std::array<double, 3>& a_rgb)
    {
        return 0.299 * a_rgb[0] + 0.587 * a_rgb[1] + 0.114 * a_rgb[2];
    }

    std::array<double, 3> RotateHuePreservingSaturationAndLuminance(
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

    void ShiftHue(RE::Color& a_color, const double a_degrees)
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

    void ShiftHue(RE::NiColor& a_color, const double a_degrees)
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

    bool BrightnessIsActive(const BrightnessValues& a_settings)
    {
        return std::abs(a_settings.ambientMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.sunlightMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.effectLightingMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.fogFarMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.fogNearMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.waterMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.skyStaticsMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.skyUpperMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.skyLowerMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.horizonMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.sunMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.sunGlareMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.moonGlareMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.starsMultiplier - 1.0) > 0.0001 ||
               std::abs(a_settings.cloudLayers - 1.0) > 0.0001;
    }

    bool AmbientCompressionIsActive(const CompressionValues& a_settings)
    {
        return std::abs(a_settings.ambientCompression) > 0.0001;
    }

    bool SunlightCompressionIsActive(const CompressionValues& a_settings)
    {
        return std::abs(a_settings.sunlightCompression) > 0.0001;
    }

    bool CompressionIsActive(const CompressionValues& a_settings)
    {
        return AmbientCompressionIsActive(a_settings) ||
               SunlightCompressionIsActive(a_settings) ||
               std::abs(a_settings.effectLightingCompression) > 0.0001 ||
               std::abs(a_settings.fogFarCompression) > 0.0001 ||
               std::abs(a_settings.fogNearCompression) > 0.0001 ||
               std::abs(a_settings.waterMultiplierCompression) > 0.0001 ||
               std::abs(a_settings.skyStaticsCompression) > 0.0001 ||
               std::abs(a_settings.skyUpperCompression) > 0.0001 ||
               std::abs(a_settings.skyLowerCompression) > 0.0001 ||
               std::abs(a_settings.horizonCompression) > 0.0001 ||
               std::abs(a_settings.sunCompression) > 0.0001 ||
               std::abs(a_settings.sunGlareCompression) > 0.0001 ||
               std::abs(a_settings.moonGlareCompression) > 0.0001 ||
               std::abs(a_settings.starsCompression) > 0.0001;
    }

    std::array<double, 16> SaturationMultipliers(const SaturationValues& a_values)
    {
        return {
            a_values.ambientMultiplier,
            a_values.sunlightMultiplier,
            a_values.effectLightingMultiplier,
            a_values.fogFarMultiplier,
            a_values.fogNearMultiplier,
            a_values.waterMultiplier,
            a_values.skyStaticsMultiplier,
            a_values.skyUpperMultiplier,
            a_values.skyLowerMultiplier,
            a_values.horizonMultiplier,
            a_values.sunMultiplier,
            a_values.sunGlareMultiplier,
            a_values.moonGlareMultiplier,
            a_values.starsMultiplier,
            a_values.cloudLayers,
            a_values.volumetricLightingMultiplier,
        };
    }

    bool SaturationValuesAreActive(const SaturationValues& a_values)
    {
        return std::ranges::any_of(SaturationMultipliers(a_values), [](const double a_value)
            { return std::abs(a_value - 1.0) > 0.0001; });
    }

    bool SaturationIsActive(const SaturationResolution& a_settings)
    {
        return SaturationValuesAreActive(a_settings.values);
    }

    bool HueScalesAreActive(const AmbientHueScaleValues& a_scales)
    {
        return std::abs(a_scales.red - 1.0) > 0.0001 ||
               std::abs(a_scales.orange - 1.0) > 0.0001 ||
               std::abs(a_scales.yellow - 1.0) > 0.0001 ||
               std::abs(a_scales.green - 1.0) > 0.0001 ||
               std::abs(a_scales.teal - 1.0) > 0.0001 ||
               std::abs(a_scales.blue - 1.0) > 0.0001 ||
               std::abs(a_scales.magenta - 1.0) > 0.0001;
    }

    template <class Bands>
    bool HueShiftBandsAreActive(const Bands& a_shifts)
    {
        return std::abs(a_shifts.red) > 0.0001 ||
               std::abs(a_shifts.orange) > 0.0001 ||
               std::abs(a_shifts.yellow) > 0.0001 ||
               std::abs(a_shifts.green) > 0.0001 ||
               std::abs(a_shifts.teal) > 0.0001 ||
               std::abs(a_shifts.blue) > 0.0001 ||
               std::abs(a_shifts.magenta) > 0.0001;
    }

    bool HueShiftIsActive(const HueShiftResolution& a_hueShift)
    {
        for (const auto& value : a_hueShift.values)
        {
            if (HueShiftBandsAreActive(value)) return true;
        }
        return false;
    }

    bool VolumetricLightingSaturationIsActive(
        const SaturationResolution& a_saturation,
        const AmbientHueScaleValues& a_hueScales)
    {
        return std::abs(a_saturation.values.volumetricLightingMultiplier - 1.0) > 0.0001 ||
               HueScalesAreActive(a_hueScales);
    }

    bool VolumetricLightingHueShiftIsActive(const HueShiftResolution& a_hueShift)
    {
        constexpr std::size_t volumetricLightingIndex = 15;
        return HueShiftBandsAreActive(a_hueShift.values[volumetricLightingIndex]);
    }

    template <class Color>
    double HueShiftDegrees(
        const std::size_t a_field,
        const Color& a_color,
        const HueShiftResolution* a_shift,
        const HueRanges& a_ranges)
    {
        const auto weights = HueBandWeights(a_color, a_ranges);
        if (!weights)
        {
            return 0.0;
        }
        double totalWeight = 0.0;
        double degrees = 0.0;
        for (std::size_t i = 0; i < weights->size(); ++i)
        {
            totalWeight += (*weights)[i];
            if (a_shift)
            {
                degrees += HueBandValue(a_shift->values[a_field], i) * (*weights)[i];
            }
        }
        return totalWeight > 0.0001 ? degrees / totalWeight : 0.0;
    }

    double ColorHueShiftDegrees(
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

    double ColorHueShiftDegrees(
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

    template <class Color>
    double SaturationHueScale(
        const Color& a_color,
        const AmbientHueScaleValues& a_hueScales,
        const HueRanges& a_hueRanges)
    {
        return HueScale(a_color, a_hueScales, a_hueRanges);
    }

    template <class Fn>
    void ForEachDALCColor(RE::BGSDirectionalAmbientLightingColors& a_dalc, Fn&& a_fn)
    {
        a_fn(a_dalc.directional.x.max);
        a_fn(a_dalc.directional.x.min);
        a_fn(a_dalc.directional.y.max);
        a_fn(a_dalc.directional.y.min);
        a_fn(a_dalc.directional.z.max);
        a_fn(a_dalc.directional.z.min);
    }

    double PeakDALCHSVValue(const RE::BGSDirectionalAmbientLightingColors& a_dalc)
    {
        return std::max({
            GainHSVValue(a_dalc.directional.x.max),
            GainHSVValue(a_dalc.directional.x.min),
            GainHSVValue(a_dalc.directional.y.max),
            GainHSVValue(a_dalc.directional.y.min),
            GainHSVValue(a_dalc.directional.z.max),
            GainHSVValue(a_dalc.directional.z.min),
        });
    }

    bool ComputeStaticWeather(const RE::TESWeather* a_weather)
    {
        if (!a_weather)
        {
            return false;
        }

        double darkestSunlight = 255.0;
        double brightestSunlight = 0.0;
        std::array<double, 6> darkestAmbient{};
        std::array<double, 6> brightestAmbient{};
        darkestAmbient.fill(255.0);
        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            const double sunlight = HSVValue(a_weather->colorData[RE::TESWeather::ColorType::kSunlight][time]);
            const auto& dalc = a_weather->directionalAmbientLightingColors[time];
            const std::array ambient{
                HSVValue(dalc.directional.x.max), HSVValue(dalc.directional.x.min),
                HSVValue(dalc.directional.y.max), HSVValue(dalc.directional.y.min),
                HSVValue(dalc.directional.z.max), HSVValue(dalc.directional.z.min),
            };
            darkestSunlight = std::min(darkestSunlight, sunlight);
            brightestSunlight = std::max(brightestSunlight, sunlight);
            for (std::size_t index = 0; index < ambient.size(); ++index)
            {
                darkestAmbient[index] = std::min(darkestAmbient[index], ambient[index]);
                brightestAmbient[index] = std::max(brightestAmbient[index], ambient[index]);
            }
        }

        constexpr double kStaticBrightnessRange = 10.0;
        if (brightestSunlight - darkestSunlight >= kStaticBrightnessRange)
        {
            return false;
        }
        for (std::size_t index = 0; index < darkestAmbient.size(); ++index)
        {
            if (brightestAmbient[index] - darkestAmbient[index] >= kStaticBrightnessRange)
            {
                return false;
            }
        }
        return true;
    }

    bool IsStaticWeather(const RE::TESWeather* a_weather);

    constexpr std::size_t kBrightnessFieldCount = 15;
    constexpr std::array<std::string_view, kBrightnessFieldCount> kBrightnessFieldNames{
        "ambient", "sunlight", "effectLighting", "fogFar", "fogNear", "water", "skyStatics", "skyUpper",
        "skyLower", "horizon", "sun", "sunGlare", "moonGlare", "stars", "cloudLayers"
    };
    constexpr std::array<RE::TESWeather::ColorType, 13> kWeatherColorTypes{
        RE::TESWeather::ColorType::kSunlight,
        RE::TESWeather::ColorType::kEffectLighting,
        RE::TESWeather::ColorType::kFogFar,
        RE::TESWeather::ColorType::kFogNear,
        RE::TESWeather::ColorType::kWaterMultiplier,
        RE::TESWeather::ColorType::kSkyStatics,
        RE::TESWeather::ColorType::kSkyUpper,
        RE::TESWeather::ColorType::kSkyLower,
        RE::TESWeather::ColorType::kHorizon,
        RE::TESWeather::ColorType::kSun,
        RE::TESWeather::ColorType::kSunGlare,
        RE::TESWeather::ColorType::kMoonGlare,
        RE::TESWeather::ColorType::kStars,
    };
    template <class Fn>
    void ForEachFieldColor(RE::TESWeather* a_weather, const std::size_t a_field, const std::uint32_t a_time, Fn&& a_fn)
    {
        if (a_field == 0)
        {
            ForEachDALCColor(a_weather->directionalAmbientLightingColors[a_time], a_fn);
            a_fn(a_weather->colorData[RE::TESWeather::ColorType::kAmbient][a_time]);
            return;
        }
        if (a_field == kBrightnessFieldCount - 1)
        {
            for (std::uint32_t layer = 0; layer < RE::TESWeather::kTotalLayers; ++layer)
            {
                a_fn(a_weather->cloudColorData[layer][a_time]);
            }
            return;
        }
        a_fn(a_weather->colorData[kWeatherColorTypes[a_field - 1]][a_time]);
    }

    double PeakAmbientFieldHSVValue(RE::TESWeather* a_weather, const std::uint32_t a_time)
    {
        return std::max(
            PeakDALCHSVValue(a_weather->directionalAmbientLightingColors[a_time]),
            GainHSVValue(a_weather->colorData[RE::TESWeather::ColorType::kAmbient][a_time]));
    }

    double AmbientZMinusHSVValue(const RE::TESWeather* a_weather, const std::uint32_t a_time)
    {
        return HSVValue(a_weather->directionalAmbientLightingColors[a_time].directional.z.min);
    }

    double DynamicBrightnessHSVValue(
        const RE::TESWeather* a_weather,
        const std::uint32_t a_time,
        const DynamicBrightnessField a_field)
    {
        return a_field == DynamicBrightnessField::ambient ?
                   AmbientZMinusHSVValue(a_weather, a_time) :
                   HSVValue(a_weather->colorData[RE::TESWeather::ColorType::kSunlight][a_time]);
    }

    struct DynamicWeatherRange
    {
        double dark = 255.0;
        double bright = 0.0;
        bool available = false;
    };

    DynamicWeatherRange AnalyzeDynamicWeatherRange(
        const RE::TESWeather* a_weather,
        const DynamicBrightnessField a_field)
    {
        DynamicWeatherRange result;
        if (!a_weather)
        {
            return result;
        }
        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            const double value = DynamicBrightnessHSVValue(a_weather, time, a_field);
            result.dark = std::min(result.dark, value);
            result.bright = std::max(result.bright, value);
            result.available = true;
        }
        return result;
    }

    DynamicAmbientRange AnalyzeDynamicAmbientRange(
        const SourceWeatherSet& a_weatherSet,
        const DynamicAmbientMode a_mode,
        const DynamicBrightnessField a_field)
    {
        DynamicAmbientRange result{ 255.0, 0.0, false };
        for (const auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }

            const auto weatherRange = AnalyzeDynamicWeatherRange(weather, a_field);
            if (!weatherRange.available || weatherRange.bright <= 0.0001)
            {
                continue;
            }

            if (a_mode == DynamicAmbientMode::between)
            {
                if (!result.available || weatherRange.bright < result.darkLimit)
                {
                    result.darkWeather = weather;
                }
                if (!result.available || weatherRange.bright > result.brightLimit)
                {
                    result.brightWeather = weather;
                }
                result.darkLimit = std::min(result.darkLimit, weatherRange.bright);
                result.brightLimit = std::max(result.brightLimit, weatherRange.bright);
                result.available = true;
                continue;
            }

            if (!result.available || weatherRange.dark < result.darkLimit)
            {
                result.darkWeather = weather;
            }
            if (!result.available || weatherRange.bright > result.brightLimit)
            {
                result.brightWeather = weather;
            }
            result.darkLimit = std::min(result.darkLimit, weatherRange.dark);
            result.brightLimit = std::max(result.brightLimit, weatherRange.bright);
            result.available = true;
        }
        if (result.available)
        {
            result.darkLimit = std::clamp(result.darkLimit, 0.0, 255.0);
            result.brightLimit = std::clamp(result.brightLimit, result.darkLimit, 255.0);
        }
        return result;
    }

    DynamicAmbientRange ResolveDynamicAmbientTarget(
        const DynamicAmbientRange& a_source,
        const DynamicAmbientSettings& a_settings)
    {
        if (!a_source.available)
        {
            return a_source;
        }
        const double first = std::clamp(a_settings.darkLimit.value_or(a_source.darkLimit), 0.0, 255.0);
        const double second = std::clamp(a_settings.brightLimit.value_or(a_source.brightLimit), 0.0, 255.0);
        return {
            .darkLimit = std::min(first, second),
            .brightLimit = std::max(first, second),
            .available = true,
        };
    }

    double RemapDynamicAmbientValue(
        const double a_value,
        const DynamicAmbientRange& a_source,
        const DynamicAmbientRange& a_target)
    {
        const double sourceSpan = a_source.brightLimit - a_source.darkLimit;
        if (sourceSpan <= 0.0001)
        {
            return (a_target.darkLimit + a_target.brightLimit) * 0.5;
        }
        const double position = std::clamp((a_value - a_source.darkLimit) / sourceSpan, 0.0, 1.0);
        return a_target.darkLimit + ((a_target.brightLimit - a_target.darkLimit) * position);
    }

    void ApplyDynamicBrightnessGain(
        RE::TESWeather* a_weather,
        const std::uint32_t a_time,
        const double a_masterGain,
        const DynamicBrightnessField a_field,
        const BrightnessResolution& a_brightness)
    {
        if (std::abs(a_masterGain - 1.0) <= 0.0001)
        {
            return;
        }

        constexpr std::size_t ambientIndex = 0;
        constexpr std::size_t sunlightIndex = 1;
        const auto masterIndex =
            a_field == DynamicBrightnessField::ambient ?
                ambientIndex :
                sunlightIndex;
        std::array<double, kBrightnessFieldCount> gains{};
        std::array<bool, kBrightnessFieldCount> resolved{};
        std::function<double(std::size_t)> resolveGain = [&](const std::size_t a_index)
        {
            if (!resolved[a_index])
            {
                if (a_index == masterIndex)
                {
                    gains[a_index] = a_masterGain;
                }
                else if (const auto link = a_brightness.links[a_index])
                {
                    gains[a_index] = std::max(
                        0.0,
                        1.0 + ((resolveGain(link->index) - 1.0) * link->scale));
                }
                else
                {
                    gains[a_index] = 1.0;
                }
                resolved[a_index] = true;
            }
            return gains[a_index];
        };

        for (std::size_t index = 0; index < kBrightnessFieldCount; ++index)
        {
            const double gain = resolveGain(index);
            if (std::abs(gain - 1.0) > 0.0001)
            {
                ForEachFieldColor(a_weather, index, a_time, [&](RE::Color& a_color)
                {
                    if (gain <= 0.0001)
                    {
                        a_color.red = 0;
                        a_color.green = 0;
                        a_color.blue = 0;
                    }
                    else
                    {
                        ApplyCompressionGain(a_color, gain);
                    }
                });
            }
        }
    }

    bool ApplyDynamicBrightnessWithin(
        const SourceWeatherSet& a_weatherSet,
        const DynamicAmbientSettings& a_settings,
        const DynamicBrightnessField a_field,
        const BrightnessResolution& a_brightness,
        DynamicBrightnessStatus& a_status)
    {
        const auto source = AnalyzeDynamicAmbientRange(a_weatherSet, DynamicAmbientMode::within, a_field);
        a_status = {
            .source = source,
            .result = source,
            .compression =
                source.available && source.brightLimit - source.darkLimit > 0.0001 ?
                    std::optional{ 0.0 } :
                    std::nullopt,
            .available = source.available,
        };
        if (!source.available || (!a_settings.darkLimit && !a_settings.brightLimit))
        {
            return false;
        }
        const auto target = ResolveDynamicAmbientTarget(source, a_settings);

        const auto darkReference = AnalyzeDynamicWeatherRange(source.darkWeather, a_field);
        const auto brightReference = AnalyzeDynamicWeatherRange(source.brightWeather, a_field);
        if (!darkReference.available || !brightReference.available)
        {
            return false;
        }
        const double darkReferenceSpan = darkReference.bright - source.darkLimit;
        const double brightReferenceSpan = source.brightLimit - brightReference.dark;
        double lowerPosition =
            darkReferenceSpan > 0.0001 ?
                (target.darkLimit - source.darkLimit) / darkReferenceSpan :
                0.0;
        double upperPosition =
            brightReferenceSpan > 0.0001 ?
                (target.brightLimit - brightReference.dark) / brightReferenceSpan :
                1.0;

        double minimumPosition = -std::numeric_limits<double>::infinity();
        double maximumPosition = std::numeric_limits<double>::infinity();
        for (const auto* weather : a_weatherSet)
        {
            const auto weatherRange = AnalyzeDynamicWeatherRange(weather, a_field);
            const double weatherSpan = weatherRange.bright - weatherRange.dark;
            if (weatherRange.available && weatherSpan > 0.0001)
            {
                minimumPosition = std::max(minimumPosition, -weatherRange.dark / weatherSpan);
                maximumPosition = std::min(maximumPosition, (255.0 - weatherRange.dark) / weatherSpan);
            }
        }
        lowerPosition = std::max(lowerPosition, minimumPosition);
        upperPosition = std::min(upperPosition, maximumPosition);
        if (lowerPosition > upperPosition)
        {
            const double midpoint = std::clamp(
                (lowerPosition + upperPosition) * 0.5,
                minimumPosition,
                maximumPosition);
            lowerPosition = midpoint;
            upperPosition = midpoint;
        }

        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }
            const auto weatherRange = AnalyzeDynamicWeatherRange(weather, a_field);
            const double weatherSpan = weatherRange.bright - weatherRange.dark;
            if (!weatherRange.available || weatherSpan <= 0.0001)
            {
                continue;
            }

            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                const double before = DynamicBrightnessHSVValue(weather, time, a_field);
                const double position = std::clamp((before - weatherRange.dark) / weatherSpan, 0.0, 1.0);
                const double mappedPosition =
                    lowerPosition + ((upperPosition - lowerPosition) * position);
                const double mapped = weatherRange.dark + (weatherSpan * mappedPosition);
                ApplyDynamicBrightnessGain(
                    weather,
                    time,
                    mapped / std::max(1.0, before),
                    a_field,
                    a_brightness);
            }
        }
        a_status.result = AnalyzeDynamicAmbientRange(a_weatherSet, DynamicAmbientMode::within, a_field);
        a_status.compression = 100.0 * (1.0 - (upperPosition - lowerPosition));
        return true;
    }

    bool ApplyDynamicBrightnessBetween(
        const SourceWeatherSet& a_weatherSet,
        const DynamicAmbientSettings& a_settings,
        const DynamicBrightnessField a_field,
        const BrightnessResolution& a_brightness,
        DynamicBrightnessStatus& a_status)
    {
        const auto source = AnalyzeDynamicAmbientRange(a_weatherSet, DynamicAmbientMode::between, a_field);
        a_status = {
            .source = source,
            .result = source,
            .compression =
                source.available && source.brightLimit - source.darkLimit > 0.0001 ?
                    std::optional{ 0.0 } :
                    std::nullopt,
            .available = source.available,
        };
        if (!source.available || (!a_settings.darkLimit && !a_settings.brightLimit))
        {
            return false;
        }
        const auto target = ResolveDynamicAmbientTarget(source, a_settings);

        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }
            double weatherPeak = 0.0;
            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                weatherPeak = std::max(weatherPeak, DynamicBrightnessHSVValue(weather, time, a_field));
            }
            if (weatherPeak <= 0.0001)
            {
                continue;
            }

            const double gain = RemapDynamicAmbientValue(weatherPeak, source, target) / weatherPeak;
            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                ApplyDynamicBrightnessGain(weather, time, gain, a_field, a_brightness);
            }
        }
        a_status.result = AnalyzeDynamicAmbientRange(a_weatherSet, DynamicAmbientMode::between, a_field);
        const double sourceSpan = source.brightLimit - source.darkLimit;
        const double resultSpan = a_status.result.brightLimit - a_status.result.darkLimit;
        a_status.compression =
            sourceSpan > 0.0001 ?
                std::optional{ 100.0 * (1.0 - resultSpan / sourceSpan) } :
                std::nullopt;
        return true;
    }

    std::array<double, kBrightnessFieldCount> BaseBrightnessMultipliers(const BrightnessValues& a_values)
    {
        return {
            a_values.ambientMultiplier, a_values.sunlightMultiplier, a_values.effectLightingMultiplier,
            a_values.fogFarMultiplier, a_values.fogNearMultiplier, a_values.waterMultiplier,
            a_values.skyStaticsMultiplier, a_values.skyUpperMultiplier, a_values.skyLowerMultiplier,
            a_values.horizonMultiplier, a_values.sunMultiplier, a_values.sunGlareMultiplier,
            a_values.moonGlareMultiplier, a_values.starsMultiplier, a_values.cloudLayers
        };
    }

    double ConstrainMasterGain(
        const SourceWeatherSet& a_weatherSet,
        const std::size_t a_field,
        const double a_requestedGain)
    {
        const double requestedGain = std::max(0.1, a_requestedGain);
        if (std::abs(requestedGain - 1.0) <= 0.0001)
        {
            return 1.0;
        }

        double maximumValue = 0.0;
        double minimumFloorValue = 256.0;
        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }

            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                const auto inspectColor = [&](const RE::Color& a_color)
                    {
                        const double value = HSVValue(a_color);
                        maximumValue = std::max(maximumValue, value);
                        if (value >= 10.0)
                        {
                            minimumFloorValue = std::min(minimumFloorValue, value);
                        }
                    };

                if (a_field == 0)
                {
                ForEachDALCColor(weather->directionalAmbientLightingColors[time], inspectColor);
                    inspectColor(weather->colorData[RE::TESWeather::ColorType::kAmbient][time]);
                }
                else
                {
                    ForEachFieldColor(weather, a_field, time, inspectColor);
                }
            }
        }

        if (maximumValue <= 0.0)
        {
            return 1.0;
        }
        if (requestedGain > 1.0)
        {
            return std::min(requestedGain, 255.0 / maximumValue);
        }
        return minimumFloorValue <= 255.0 ? std::max(requestedGain, 1.0 / minimumFloorValue) : requestedGain;
    }

    void ApplyBrightness(
        const SourceWeatherSet& a_weatherSet,
        const BrightnessResolution& a_brightness)
    {
        const auto requested = BaseBrightnessMultipliers(a_brightness.values);
        std::array<double, kBrightnessFieldCount> gains{};
        std::array<bool, kBrightnessFieldCount> resolved{};
        std::array<bool, kBrightnessFieldCount> resolving{};

        std::function<double(std::size_t)> resolveGain = [&](const std::size_t a_field)
        {
            if (resolved[a_field])
            {
                return gains[a_field];
            }
            if (resolving[a_field])
            {
                logger::warn("Circular brightness gain link at {}", kBrightnessFieldNames[a_field]);
                return 1.0;
            }

            resolving[a_field] = true;
            const auto link = a_brightness.links[a_field];
            if (link && link->index < kBrightnessFieldCount)
            {
                const double masterGain = resolveGain(link->index);
                gains[a_field] = std::max(0.1, 1.0 + ((masterGain - 1.0) * link->scale));
            }
            else
            {
                gains[a_field] = ConstrainMasterGain(a_weatherSet, a_field, requested[a_field]);
                if (std::abs(gains[a_field] - std::max(0.0, requested[a_field])) > 0.0001)
                {
                    DetailedLogging::Info(
                        "Brightness master {} requested {:.4f}x, constrained to {:.4f}x by the shared profile floor/ceiling",
                        kBrightnessFieldNames[a_field],
                        requested[a_field],
                        gains[a_field]);
                }
            }
            resolving[a_field] = false;
            resolved[a_field] = true;
            return gains[a_field];
        };

        for (std::size_t field = 0; field < kBrightnessFieldCount; ++field)
        {
            resolveGain(field);
        }

        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }
            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                for (std::size_t field = 0; field < kBrightnessFieldCount; ++field)
                {
                    if (std::abs(gains[field] - 1.0) <= 0.0001)
                    {
                        continue;
                    }
                    ForEachFieldColor(weather, field, time, [&](RE::Color& a_color)
                        { MultiplyBrightnessColor(a_color, gains[field]); });
                }
            }
        }
    }

    void ApplyCompression(
        RE::TESWeather* a_weather,
        const CompressionResolution& a_compression,
        const AnchorValues& a_anchors)
    {
        constexpr std::size_t ambientIndex = 0;
        constexpr std::size_t fieldCount = 14;
        const auto& values = a_compression.values;
        const std::array<double, fieldCount> compression{
            std::clamp(values.ambientCompression / 100.0, -2.0, 2.0),
            std::clamp(values.sunlightCompression / 100.0, -2.0, 2.0),
            std::clamp(values.effectLightingCompression / 100.0, -2.0, 2.0),
            std::clamp(values.fogFarCompression / 100.0, -2.0, 2.0),
            std::clamp(values.fogNearCompression / 100.0, -2.0, 2.0),
            std::clamp(values.waterMultiplierCompression / 100.0, -2.0, 2.0),
            std::clamp(values.skyStaticsCompression / 100.0, -2.0, 2.0),
            std::clamp(values.skyUpperCompression / 100.0, -2.0, 2.0),
            std::clamp(values.skyLowerCompression / 100.0, -2.0, 2.0),
            std::clamp(values.horizonCompression / 100.0, -2.0, 2.0),
            std::clamp(values.sunCompression / 100.0, -2.0, 2.0),
            std::clamp(values.sunGlareCompression / 100.0, -2.0, 2.0),
            std::clamp(values.moonGlareCompression / 100.0, -2.0, 2.0),
            std::clamp(values.starsCompression / 100.0, -2.0, 2.0),
        };
        const std::array<double, fieldCount> anchors{
            a_anchors.ambientAnchor,
            a_anchors.sunlightAnchor,
            a_anchors.effectLightingAnchor,
            a_anchors.fogFarAnchor,
            a_anchors.fogNearAnchor,
            a_anchors.waterMultiplierAnchor,
            a_anchors.skyStaticsAnchor,
            a_anchors.skyUpperAnchor,
            a_anchors.skyLowerAnchor,
            a_anchors.horizonAnchor,
            a_anchors.sunAnchor,
            a_anchors.sunGlareAnchor,
            a_anchors.moonGlareAnchor,
            a_anchors.starsAnchor,
        };
        std::array<std::array<double, RE::TESWeather::ColorTime::kTotal>, fieldCount> gains{};
        for (auto& fieldGains : gains)
        {
            fieldGains.fill(1.0);
        }
        std::array<bool, fieldCount> applied{};

        std::function<void(std::size_t)> applyField = [&](const std::size_t a_index)
        {
            if (applied[a_index])
            {
                return;
            }

            const auto link = a_compression.links[a_index];
            if (link)
            {
                applyField(link->index);
            }
            else if (std::abs(compression[a_index]) <= 0.0001)
            {
                applied[a_index] = true;
                return;
            }

            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                const double effectiveAnchor = anchors[a_index];
                if (a_index == ambientIndex)
                {
                    auto& dalc = a_weather->directionalAmbientLightingColors[time];
                    auto& ambient = a_weather->colorData[RE::TESWeather::ColorType::kAmbient][time];
                    const double before = PeakAmbientFieldHSVValue(a_weather, time);
                    if (link)
                    {
                        const double sourceGain = 1.0 + ((gains[link->index][time] - 1.0) * link->scale);
                        if (std::abs(sourceGain - 1.0) > 0.0001)
                        {
                            ForEachDALCColor(dalc, [&](RE::Color& a_color)
                                { ApplyCompressionGain(a_color, sourceGain); });
                            ApplyCompressionGain(ambient, sourceGain);
                        }
                    }
                    else
                    {
                        ForEachDALCColor(dalc, [&](RE::Color& a_color)
                            { CompressColor(a_color, compression[a_index], effectiveAnchor); });
                        CompressColor(ambient, compression[a_index], effectiveAnchor);
                    }

                    gains[a_index][time] = PeakAmbientFieldHSVValue(a_weather, time) / before;
                    continue;
                }

                auto& color = a_weather->colorData[kWeatherColorTypes[a_index - 1]][time];
                const double before = GainHSVValue(color);
                if (link)
                {
                    const double sourceGain = 1.0 + ((gains[link->index][time] - 1.0) * link->scale);
                    if (std::abs(sourceGain - 1.0) > 0.0001)
                    {
                        ApplyCompressionGain(color, sourceGain);
                    }
                }
                else
                {
                    CompressColor(color, compression[a_index], effectiveAnchor);
                }

                gains[a_index][time] = GainHSVValue(color) / before;
            }

            applied[a_index] = true;
        };

        for (std::size_t i = 0; i < fieldCount; ++i)
        {
            applyField(i);
        }
    }

    std::array<double, 14> CompressionAmounts(const CompressionValues& a_values)
    {
        return {
            a_values.ambientCompression,
            a_values.sunlightCompression,
            a_values.effectLightingCompression,
            a_values.fogFarCompression,
            a_values.fogNearCompression,
            a_values.waterMultiplierCompression,
            a_values.skyStaticsCompression,
            a_values.skyUpperCompression,
            a_values.skyLowerCompression,
            a_values.horizonCompression,
            a_values.sunCompression,
            a_values.sunGlareCompression,
            a_values.moonGlareCompression,
            a_values.starsCompression,
        };
    }

    double FieldPeakHSVValue(RE::TESWeather* a_weather, const std::size_t a_field, const std::uint32_t a_time)
    {
        double peak = 0.0;
        ForEachFieldColor(a_weather, a_field, a_time, [&](const RE::Color& a_color)
            { peak = std::max(peak, GainHSVValue(a_color)); });
        return std::max(1.0, peak);
    }

    void ApplyWithinWeatherCompression(
        RE::TESWeather* a_weather,
        const CompressionResolution& a_compression)
    {
        constexpr std::size_t fieldCount = 14;
        const auto rawAmounts = CompressionAmounts(a_compression.values);
        std::array<double, fieldCount> compression{};
        std::array<double, fieldCount> anchors{};
        for (std::size_t field = 0; field < fieldCount; ++field)
        {
            compression[field] = std::clamp(rawAmounts[field] / 100.0, -2.0, 2.0);
            anchors[field] = 1.0;
            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                anchors[field] = std::max(anchors[field], FieldPeakHSVValue(a_weather, field, time));
            }
        }

        std::array<std::array<double, RE::TESWeather::ColorTime::kTotal>, fieldCount> gains{};
        for (auto& fieldGains : gains)
        {
            fieldGains.fill(1.0);
        }
        std::array<bool, fieldCount> applied{};

        std::function<void(std::size_t)> applyField = [&](const std::size_t a_field)
        {
            if (applied[a_field])
            {
                return;
            }

            const auto link = a_compression.links[a_field];
            if (link)
            {
                applyField(link->index);
            }
            else if (std::abs(compression[a_field]) <= 0.0001)
            {
                applied[a_field] = true;
                return;
            }

            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                if (a_field == 0)
                {
                    auto& dalc = a_weather->directionalAmbientLightingColors[time];
                    auto& ambient = a_weather->colorData[RE::TESWeather::ColorType::kAmbient][time];
                    const double before = PeakAmbientFieldHSVValue(a_weather, time);
                    if (link)
                    {
                        const double sourceGain = 1.0 + ((gains[link->index][time] - 1.0) * link->scale);
                        ForEachDALCColor(dalc, [&](RE::Color& a_color)
                            { ApplyCompressionGain(a_color, sourceGain); });
                        ApplyCompressionGain(ambient, sourceGain);
                    }
                    else
                    {
                        ForEachDALCColor(dalc, [&](RE::Color& a_color)
                            { CompressColor(a_color, compression[a_field], anchors[a_field]); });
                        CompressColor(ambient, compression[a_field], anchors[a_field]);
                    }
                    gains[a_field][time] = PeakAmbientFieldHSVValue(a_weather, time) / before;
                    continue;
                }

                auto& color = a_weather->colorData[kWeatherColorTypes[a_field - 1]][time];
                const double before = GainHSVValue(color);
                if (link)
                {
                    const double sourceGain = 1.0 + ((gains[link->index][time] - 1.0) * link->scale);
                    ApplyCompressionGain(color, sourceGain);
                }
                else
                {
                    CompressColor(color, compression[a_field], anchors[a_field]);
                }
                gains[a_field][time] = GainHSVValue(color) / before;
            }

            applied[a_field] = true;
        };

        for (std::size_t field = 0; field < fieldCount; ++field)
        {
            applyField(field);
        }
    }

    void ApplySaturation(
        RE::TESWeather* a_weather,
        const SaturationResolution& a_settings,
        const AmbientHueScaleValues& a_hueScales,
        const HueRanges& a_hueRanges)
    {
        const auto values = SaturationMultipliers(a_settings.values);
        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            const double ambientMultiplier = values[0];
            ForEachDALCColor(a_weather->directionalAmbientLightingColors[time], [&](RE::Color& a_color)
                {
                    const double effectiveMultiplier = ambientMultiplier * SaturationHueScale(a_color, a_hueScales, a_hueRanges);
                    if (std::abs(effectiveMultiplier - 1.0) > 0.0001)
                    {
                        SaturateColor(a_color, effectiveMultiplier);
                    } });
            auto& ambientColor = a_weather->colorData[RE::TESWeather::ColorType::kAmbient][time];
            const double ambientColorMultiplier = ambientMultiplier * SaturationHueScale(ambientColor, a_hueScales, a_hueRanges);
            if (std::abs(ambientColorMultiplier - 1.0) > 0.0001)
            {
                SaturateColor(ambientColor, ambientColorMultiplier);
            }

            for (std::size_t field = 1; field <= kWeatherColorTypes.size(); ++field)
            {
                auto& color = a_weather->colorData[kWeatherColorTypes[field - 1]][time];
                const double multiplier = values[field] * SaturationHueScale(color, a_hueScales, a_hueRanges);
                if (std::abs(multiplier - 1.0) > 0.0001)
                {
                    SaturateColor(color, multiplier);
                }
            }

            const double cloudLayersMultiplier = values[14];
            for (std::uint32_t layer = 0; layer < RE::TESWeather::kTotalLayers; ++layer)
            {
                auto& color = a_weather->cloudColorData[layer][time];
                const double multiplier = cloudLayersMultiplier * SaturationHueScale(color, a_hueScales, a_hueRanges);
                if (std::abs(multiplier - 1.0) > 0.0001)
                {
                    SaturateColor(color, multiplier);
                }
            }
        }
    }

    void ApplyHueShift(
        RE::TESWeather* a_weather,
        const HueShiftResolution& a_hueShift,
        const HueRanges& a_hueRanges)
    {
        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            ForEachDALCColor(a_weather->directionalAmbientLightingColors[time], [&](RE::Color& a_color)
                { ShiftHue(a_color, HueShiftDegrees(0, a_color, &a_hueShift, a_hueRanges)); });
            auto& ambient = a_weather->colorData[RE::TESWeather::ColorType::kAmbient][time];
            ShiftHue(ambient, HueShiftDegrees(0, ambient, &a_hueShift, a_hueRanges));

            for (std::size_t field = 1; field <= 13; ++field)
            {
                auto& color = a_weather->colorData[kWeatherColorTypes[field - 1]][time];
                ShiftHue(color, HueShiftDegrees(field, color, &a_hueShift, a_hueRanges));
            }

            for (std::uint32_t layer = 0; layer < RE::TESWeather::kTotalLayers; ++layer)
            {
                auto& color = a_weather->cloudColorData[layer][time];
                ShiftHue(color, HueShiftDegrees(14, color, &a_hueShift, a_hueRanges));
            }
        }
    }

    void CaptureBaselineIfNeeded(RE::TESWeather* a_weather)
    {
        auto sta = MPL::Config::StatData::GetSingleton();
        if (sta->weatherBaselines.contains(a_weather))
        {
            return;
        }

        WeatherBaseline baseline;
        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            baseline.dalc[time] = a_weather->directionalAmbientLightingColors[time];
            baseline.ambient[time] = a_weather->colorData[RE::TESWeather::ColorType::kAmbient][time];
            baseline.sunlight[time] = a_weather->colorData[RE::TESWeather::ColorType::kSunlight][time];
            baseline.effectLighting[time] = a_weather->colorData[RE::TESWeather::ColorType::kEffectLighting][time];
            baseline.fogFar[time] = a_weather->colorData[RE::TESWeather::ColorType::kFogFar][time];
            baseline.fogNear[time] = a_weather->colorData[RE::TESWeather::ColorType::kFogNear][time];
            baseline.waterMultiplier[time] = a_weather->colorData[RE::TESWeather::ColorType::kWaterMultiplier][time];
            baseline.skyStatics[time] = a_weather->colorData[RE::TESWeather::ColorType::kSkyStatics][time];
            baseline.skyUpper[time] = a_weather->colorData[RE::TESWeather::ColorType::kSkyUpper][time];
            baseline.skyLower[time] = a_weather->colorData[RE::TESWeather::ColorType::kSkyLower][time];
            baseline.horizon[time] = a_weather->colorData[RE::TESWeather::ColorType::kHorizon][time];
            baseline.sun[time] = a_weather->colorData[RE::TESWeather::ColorType::kSun][time];
            baseline.sunGlare[time] = a_weather->colorData[RE::TESWeather::ColorType::kSunGlare][time];
            baseline.moonGlare[time] = a_weather->colorData[RE::TESWeather::ColorType::kMoonGlare][time];
            baseline.stars[time] = a_weather->colorData[RE::TESWeather::ColorType::kStars][time];
            for (std::uint32_t layer = 0; layer < RE::TESWeather::kTotalLayers; ++layer)
            {
                baseline.cloudLayers[layer][time] = a_weather->cloudColorData[layer][time];
            }
        }
        sta->weatherBaselines.emplace(a_weather, baseline);
    }

    void RestoreBaseline(RE::TESWeather* a_weather)
    {
        auto sta = MPL::Config::StatData::GetSingleton();
        auto it = sta->weatherBaselines.find(a_weather);
        if (it == sta->weatherBaselines.end())
        {
            return;
        }

        const auto& baseline = it->second;
        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            a_weather->directionalAmbientLightingColors[time] = baseline.dalc[time];
            a_weather->colorData[RE::TESWeather::ColorType::kAmbient][time] = baseline.ambient[time];
            a_weather->colorData[RE::TESWeather::ColorType::kSunlight][time] = baseline.sunlight[time];
            a_weather->colorData[RE::TESWeather::ColorType::kEffectLighting][time] = baseline.effectLighting[time];
            a_weather->colorData[RE::TESWeather::ColorType::kFogFar][time] = baseline.fogFar[time];
            a_weather->colorData[RE::TESWeather::ColorType::kFogNear][time] = baseline.fogNear[time];
            a_weather->colorData[RE::TESWeather::ColorType::kWaterMultiplier][time] = baseline.waterMultiplier[time];
            a_weather->colorData[RE::TESWeather::ColorType::kSkyStatics][time] = baseline.skyStatics[time];
            a_weather->colorData[RE::TESWeather::ColorType::kSkyUpper][time] = baseline.skyUpper[time];
            a_weather->colorData[RE::TESWeather::ColorType::kSkyLower][time] = baseline.skyLower[time];
            a_weather->colorData[RE::TESWeather::ColorType::kHorizon][time] = baseline.horizon[time];
            a_weather->colorData[RE::TESWeather::ColorType::kSun][time] = baseline.sun[time];
            a_weather->colorData[RE::TESWeather::ColorType::kSunGlare][time] = baseline.sunGlare[time];
            a_weather->colorData[RE::TESWeather::ColorType::kMoonGlare][time] = baseline.moonGlare[time];
            a_weather->colorData[RE::TESWeather::ColorType::kStars][time] = baseline.stars[time];
            for (std::uint32_t layer = 0; layer < RE::TESWeather::kTotalLayers; ++layer)
            {
                a_weather->cloudColorData[layer][time] = baseline.cloudLayers[layer][time];
            }
        }
    }

    void RestoreAllCapturedWeatherBaselines()
    {
        auto* stat = MPL::Config::StatData::GetSingleton();
        for (const auto& [weather, baseline] : stat->weatherBaselines)
        {
            (void) baseline;
            RestoreBaseline(weather);
        }
        for (const auto& [volumetricLighting, intensity] : stat->volumetricLightingIntensityBaselines)
        {
            if (volumetricLighting)
            {
                volumetricLighting->intensity = intensity;
            }
        }
        for (const auto& [volumetricLighting, color] : stat->volumetricLightingColorBaselines)
        {
            if (volumetricLighting)
            {
                volumetricLighting->color = color;
            }
        }
    }

    using ChangedVolumetricLightingSet = std::unordered_set<RE::BGSVolumetricLighting*>;

    ChangedVolumetricLightingSet ApplyVolumetricLightingSettings(
        const SourceWeatherSet& a_weatherSet,
        double a_intensityMultiplier,
        const SaturationResolution& a_saturation,
        const AmbientHueScaleValues& a_hueScales,
        const HueShiftResolution& a_hueShift,
        const HueRanges& a_hueRanges)
    {
        auto* stat = MPL::Config::StatData::GetSingleton();
        std::unordered_set<RE::BGSVolumetricLighting*> records;
        ChangedVolumetricLightingSet changed;
        const double intensityMultiplier = std::max(0.0, a_intensityMultiplier);
        const bool intensityActive = std::abs(intensityMultiplier - 1.0) > 0.0001;
        const bool colorActive = VolumetricLightingSaturationIsActive(a_saturation, a_hueScales) ||
                                 VolumetricLightingHueShiftIsActive(a_hueShift);
        if (!intensityActive && !colorActive)
        {
            return changed;
        }
        constexpr double maximum = std::numeric_limits<float>::max();

        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }

            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                auto* volumetricLighting = weather->volumetricLighting[time];
                if (!volumetricLighting)
                {
                    continue;
                }
                records.insert(volumetricLighting);
            }
        }

        for (auto* volumetricLighting : records)
        {
            bool intensityChanged = false;
            if (intensityActive)
            {
                const auto intensityBaseline = stat->volumetricLightingIntensityBaselines
                                                   .try_emplace(volumetricLighting, volumetricLighting->intensity)
                                                   .first;
                const auto adjustedIntensity = static_cast<float>(std::clamp(
                    static_cast<double>(intensityBaseline->second) * intensityMultiplier,
                    -maximum,
                    maximum));
                intensityChanged = std::abs(volumetricLighting->intensity - adjustedIntensity) > 0.0001f;
                if (intensityChanged)
                {
                    volumetricLighting->intensity = adjustedIntensity;
                }
            }

            bool colorChanged = false;
            if (colorActive)
            {
                const auto colorBaseline = stat->volumetricLightingColorBaselines
                                               .try_emplace(volumetricLighting, volumetricLighting->color)
                                               .first;
                const double colorMultiplier = a_saturation.values.volumetricLightingMultiplier *
                                               SaturationHueScale(colorBaseline->second, a_hueScales, a_hueRanges);
                auto adjustedColor = colorBaseline->second;
                if (std::abs(colorMultiplier - 1.0) > 0.0001)
                {
                    SaturateColor(adjustedColor, colorMultiplier);
                }
                ShiftHue(adjustedColor, HueShiftDegrees(15, adjustedColor, &a_hueShift, a_hueRanges));
                colorChanged = std::abs(volumetricLighting->color.red - adjustedColor.red) > 0.0001f ||
                               std::abs(volumetricLighting->color.green - adjustedColor.green) > 0.0001f ||
                               std::abs(volumetricLighting->color.blue - adjustedColor.blue) > 0.0001f;
                if (colorChanged)
                {
                    volumetricLighting->color = adjustedColor;
                }
            }

            if (intensityChanged || colorChanged)
            {
                changed.insert(volumetricLighting);
            }
        }

        return changed;
    }

    bool ReferencesChangedVolumetricLighting(
        const RE::TESWeather* a_weather,
        const ChangedVolumetricLightingSet& a_changed)
    {
        if (!a_weather)
        {
            return false;
        }

        for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
        {
            if (a_changed.contains(a_weather->volumetricLighting[time]))
            {
                return true;
            }
        }
        return false;
    }

    static std::string WeatherEditorID(const RE::TESWeather* a_weather)
    {
        if (!a_weather)
        {
            return {};
        }

        auto* stat = Config::StatData::GetSingleton();
        if (!stat->mmsfAPI)
        {
            stat->mmsfAPI = API::MMSF::RequestMMSFAPI();
        }
        return stat->mmsfAPI ? stat->mmsfAPI->LookupEDIDForFormID(a_weather->GetFormID()) : std::string{};
    }

    std::string WeatherName(const RE::TESWeather* a_weather)
    {
        auto editorID = WeatherEditorID(a_weather);
        return editorID.empty() ? "<no editor ID>" : std::move(editorID);
    }

    std::optional<std::string> ReadJsonText(const std::filesystem::path& a_fileName)
    {
        std::ifstream file(a_fileName, std::ios::binary);
        if (!file)
        {
            logger::warn("Could not open TuningUtil JSON file {}", a_fileName.string());
            return std::nullopt;
        }

        std::string text(std::istreambuf_iterator<char>(file), {});
        constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";
        if (text.starts_with(utf8Bom))
        {
            text.erase(0, utf8Bom.size());
            logger::warn("Ignored UTF-8 byte order mark in {}", a_fileName.string());
        }
        return text;
    }

    std::string ProfileNameFromKey(const std::string& a_profileName)
    {
        auto profileName = std::filesystem::path(a_profileName).filename().string();
        if (profileName.empty() || std::ranges::all_of(profileName, [](const unsigned char a_character)
                                       { return std::isspace(a_character) != 0; }))
        {
            return {};
        }

        const auto extension = std::filesystem::path(profileName).extension().string();
        if (Config::IEquals(extension, ".esp") || Config::IEquals(extension, ".esm") || Config::IEquals(extension, ".esl"))
        {
            profileName = std::filesystem::path(profileName).stem().string();
        }

        return profileName;
    }

    std::unordered_map<std::string, std::vector<std::string>>& GetPresetCatalogs()
    {
        static std::unordered_map<std::string, std::vector<std::string>> catalogs;
        return catalogs;
    }

    struct ActivePresetCache
    {
        std::vector<ActivePreset> presets;
        std::string settings{ "{}" };
    };

    std::unordered_map<std::string, ActivePresetCache>& GetActivePresetCaches()
    {
        static std::unordered_map<std::string, ActivePresetCache> caches;
        return caches;
    }

    struct PresetPreviewCache
    {
        std::vector<ActivePreset> presets;
        std::unordered_set<std::string> changedCategories;
        std::string settings{ "{}" };
        std::string changedSettings{ "{}" };
    };

    std::unordered_map<std::string, PresetPreviewCache>& GetPresetPreviewCaches()
    {
        static std::unordered_map<std::string, PresetPreviewCache> caches;
        return caches;
    }

    std::string LowercaseKey(std::string a_value)
    {
        std::ranges::transform(
            a_value,
            a_value.begin(),
            [](const unsigned char a_character)
            {
                return static_cast<char>(std::tolower(a_character));
            });
        return a_value;
    }

    std::unordered_map<std::string, DynamicBrightnessStatus>& GetDynamicBrightnessStatuses()
    {
        static std::unordered_map<std::string, DynamicBrightnessStatus> statuses;
        return statuses;
    }

    std::string DynamicBrightnessStatusKey(
        const std::string_view a_profile,
        const DynamicAmbientMode a_mode,
        const DynamicBrightnessField a_field)
    {
        return std::format(
            "{}|{}|{}",
            LowercaseKey(std::string(a_profile)),
            a_field == DynamicBrightnessField::ambient ? "ambient" : "sunlight",
            a_mode == DynamicAmbientMode::within ? "within" : "between");
    }

    void CacheDynamicBrightnessStatus(
        const std::span<const std::string> a_profiles,
        const DynamicAmbientMode a_mode,
        const DynamicBrightnessField a_field,
        const DynamicBrightnessStatus& a_status)
    {
        auto& statuses = GetDynamicBrightnessStatuses();
        for (const auto& profile : a_profiles)
        {
            statuses[DynamicBrightnessStatusKey(profile, a_mode, a_field)] = a_status;
        }
    }

    void InvalidatePresetCatalog(const std::string& a_profileName, const std::string& a_category)
    {
        const auto profileKey = LowercaseKey(a_profileName);
        auto& catalogs = GetPresetCatalogs();
        catalogs.erase(profileKey + "|categories");
        catalogs.erase(profileKey + "|presets|" + LowercaseKey(a_category));
    }

    std::optional<Settings> LoadSettings(std::string& a_sourceFile)
    {
        return TuningUtil::GetSettings(a_sourceFile);
    }

    Settings& GetOrCreateSettings(std::string& a_sourceFile)
    {
        return TuningUtil::GetSettings(a_sourceFile);
    }

    void AppendUniquePlugins(
        std::vector<std::string>& a_target,
        const std::vector<std::string>& a_source)
    {
        for (const auto& pluginName : a_source)
        {
            if (!pluginName.empty() && !std::ranges::any_of(a_target, [&](const std::string& name)
                                           { return Config::IEquals(name, pluginName); }))
            {
                a_target.push_back(pluginName);
            }
        }
    }

    bool PluginFilterEmpty(const TuningUtil::PluginFilter& a_filter)
    {
        return a_filter.exact.empty() && a_filter.contains.empty();
    }

    bool PluginNameMatches(
        const std::string_view a_pluginName,
        const TuningUtil::PluginFilter& a_filter)
    {
        if (std::ranges::any_of(a_filter.exact, [&](const auto& a_name)
                { return Config::IEquals(a_pluginName, a_name); }))
        {
            return true;
        }

        const auto pluginName = LowercaseKey(std::string(a_pluginName));
        return std::ranges::any_of(a_filter.contains, [&](const auto& a_fragment)
        {
            const auto fragment = LowercaseKey(a_fragment);
            return !fragment.empty() && pluginName.contains(fragment);
        });
    }

    const SourceWeatherSet* FindWeatherSet(
        const std::unordered_map<std::string, SourceWeatherSet>& a_weatherSets,
        const std::string& a_sourceFile)
    {
        if (const auto found = a_weatherSets.find(a_sourceFile); found != a_weatherSets.end())
        {
            return std::addressof(found->second);
        }

        const auto found = std::ranges::find_if(a_weatherSets, [&](const auto& entry)
            { return Config::IEquals(entry.first, a_sourceFile); });
        if (found != a_weatherSets.end())
        {
            return std::addressof(found->second);
        }

        return nullptr;
    }

    struct ProfilePluginTargets
    {
        bool catchAll = true;
        std::vector<std::string> included;
        std::vector<std::string> excluded;
    };

    ProfilePluginTargets ResolveProfilePluginTargets(
        const Settings& a_settings,
        const std::unordered_map<std::string, SourceWeatherSet>& a_weatherSets)
    {
        ProfilePluginTargets result;
        result.catchAll = PluginFilterEmpty(a_settings.pluginInclusions);
        for (const auto& [pluginName, weathers] : a_weatherSets)
        {
            (void)weathers;
            if (PluginNameMatches(pluginName, a_settings.pluginExclusions))
            {
                result.excluded.push_back(pluginName);
            }
            else if (!result.catchAll && PluginNameMatches(pluginName, a_settings.pluginInclusions))
            {
                result.included.push_back(pluginName);
            }
        }

        const auto sortNames = [](auto& a_names)
        {
            std::ranges::sort(a_names, {}, [](const auto& a_name) { return LowercaseKey(a_name); });
        };
        sortNames(result.included);
        sortNames(result.excluded);
        return result;
    }

    std::vector<std::string> GetOrderedSettingsProfiles()
    {
        static constexpr std::array roots{
            std::string_view{ "brightnessMultiplier" },
            std::string_view{ "volumetricLightingIntensityMultiplier" },
            std::string_view{ "saturationMultiplier" },
            std::string_view{ "hueScales" },
            std::string_view{ "hueRanges" },
            std::string_view{ "hueShift" },
            std::string_view{ "betweenWeatherCompression" },
            std::string_view{ "withinWeatherCompression" },
            std::string_view{ "compressionAnchor" },
            std::string_view{ "dynamicAmbientWithin" },
            std::string_view{ "dynamicAmbientBetween" },
            std::string_view{ "dynamicSunlightWithin" },
            std::string_view{ "dynamicSunlightBetween" },
            std::string_view{ "weatherInclusions" },
            std::string_view{ "weatherExclusions" },
            std::string_view{ "pluginInclusions" },
            std::string_view{ "pluginExclusions" },
        };
        return TuningUtil::GetProfilesWithSettings(roots);
    }

    std::vector<std::string> GetOrderedFXEffectLightingProfiles()
    {
        static constexpr std::array roots{
            std::string_view{ "fxEffectLighting" },
            std::string_view{ "effectLightingInclusions" },
            std::string_view{ "effectLightingExclusions" },
            std::string_view{ "effectLightingPluginInclusions" },
            std::string_view{ "effectLightingPluginExclusions" },
        };
        return TuningUtil::GetProfilesWithSettings(roots);
    }

    void InvalidatePresetCache()
    {
        GetPresetCatalogs().clear();
        GetActivePresetCaches().clear();
        GetPresetPreviewCaches().clear();
    }

    std::unordered_map<std::string, SourceWeatherSet> BuildSourceWeatherSets(RE::TESDataHandler* a_dataHandler)
    {
        std::unordered_map<std::string, SourceWeatherSet> result;
        for (auto* weather : a_dataHandler->GetFormArray<RE::TESWeather>())
        {
            if (!weather || weather->sourceFiles.array == nullptr)
            {
                continue;
            }

            for (const auto* sourceFile : *weather->sourceFiles.array)
            {
                if (!sourceFile)
                {
                    continue;
                }

                result[std::string(sourceFile->GetFilename())].push_back(weather);
            }
        }
        return result;
    }

    SourceWeatherSet BuildProfileWeatherSet(
        const std::unordered_map<std::string, SourceWeatherSet>& a_weatherSets,
        const std::vector<std::string>& a_targetPlugins,
        const std::string& a_profileName)
    {
        SourceWeatherSet result;
        std::unordered_set<RE::FormID> includedFormIDs;

        for (const auto& sourceFile : a_targetPlugins)
        {
            auto* weatherSet = FindWeatherSet(a_weatherSets, sourceFile);
            if (!weatherSet)
            {
                logger::warn("TuningUtil profile {} targets {}, but no loaded weather records include that plugin", a_profileName, sourceFile);
                continue;
            }

            for (auto* weather : *weatherSet)
            {
                if (weather && includedFormIDs.insert(weather->GetFormID()).second)
                {
                    result.push_back(weather);
                }
            }
        }

        return result;
    }

    SourceWeatherSet BuildUnclaimedWeatherSet(
        RE::TESDataHandler* a_dataHandler,
        const std::unordered_map<std::string, SourceWeatherSet>& a_weatherSets,
        const std::vector<std::string>& a_claimedPlugins)
    {
        std::unordered_set<RE::FormID> claimedFormIDs;
        for (const auto& pluginName : a_claimedPlugins)
        {
            if (const auto* weatherSet = FindWeatherSet(a_weatherSets, pluginName))
            {
                for (const auto* weather : *weatherSet)
                {
                    if (weather)
                    {
                        claimedFormIDs.insert(weather->GetFormID());
                    }
                }
            }
        }

        SourceWeatherSet result;
        for (auto* weather : a_dataHandler->GetFormArray<RE::TESWeather>())
        {
            if (weather && !claimedFormIDs.contains(weather->GetFormID()))
            {
                result.push_back(weather);
            }
        }
        return result;
    }

    SourceWeatherSet RemovePluginExcludedWeathers(
        SourceWeatherSet a_weathers,
        const std::unordered_map<std::string, SourceWeatherSet>& a_weatherSets,
        const std::span<const std::string> a_excludedPlugins)
    {
        std::unordered_set<RE::FormID> excludedFormIDs;
        for (const auto& pluginName : a_excludedPlugins)
        {
            if (const auto* weatherSet = FindWeatherSet(a_weatherSets, pluginName))
            {
                for (const auto* weather : *weatherSet)
                {
                    if (weather) excludedFormIDs.insert(weather->GetFormID());
                }
            }
        }
        std::erase_if(a_weathers, [&](const auto* a_weather)
            { return !a_weather || excludedFormIDs.contains(a_weather->GetFormID()); });
        return a_weathers;
    }

    struct CachedProfileWeatherSet
    {
        std::vector<std::string> targetPlugins;
        SourceWeatherSet weathers;
        bool excludesTargetPlugins = false;
    };

    struct CachedResolvedWeatherFilter
    {
        TuningUtil::WeatherFilter configured;
        std::unordered_set<RE::FormID> formIDs;
    };

    struct CachedFilteredRuleForms
    {
        TuningUtil::WeatherFilter include;
        TuningUtil::WeatherFilter exclude;
        std::unordered_set<RE::FormID> includedFormIDs;
        std::unordered_set<RE::FormID> excludedFormIDs;
    };

    struct CachedPointLightWeatherExclusions
    {
        bool initialized = false;
        std::vector<std::string> configuredRegions;
        std::unordered_set<RE::FormID> regionFormIDs;
        std::unordered_set<RE::FormID> weatherFormIDs;
    };

    struct WeatherResolutionCache
    {
        RE::TESDataHandler* dataHandler = nullptr;
        bool initialized = false;
        std::unordered_map<std::string, SourceWeatherSet> sourceWeatherSets;
        std::unordered_map<std::string, CachedProfileWeatherSet> profileWeatherSets;
        std::unordered_map<RE::FormID, bool> fxClassifications;
        std::unordered_map<RE::FormID, bool> staticClassifications;
        std::unordered_map<std::string, CachedResolvedWeatherFilter> weatherInclusions;
        std::unordered_map<std::string, CachedResolvedWeatherFilter> weatherExclusions;
        std::unordered_map<std::string, CachedFilteredRuleForms> filteredRuleForms;
        std::unordered_map<std::string, CachedPointLightWeatherExclusions>
            pointLightWeatherExclusions;
        std::unordered_set<std::string> loggedFXFilterProfiles;
        std::unordered_set<std::string> loggedWeatherEnumerations;
    };

    bool EditorIDContainsFX(const RE::TESWeather* a_weather)
    {
        const auto editorID = WeatherEditorID(a_weather);
        for (std::size_t index = 0; index + 1 < editorID.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>(editorID[index])) == 'f' &&
                std::tolower(static_cast<unsigned char>(editorID[index + 1])) == 'x')
            {
                return true;
            }
        }
        return false;
    }

    bool EditorIDContainsAny(
        const RE::TESWeather* a_weather,
        const std::span<const std::string> a_fragments)
    {
        const auto editorID = LowercaseKey(WeatherEditorID(a_weather));
        return !editorID.empty() && std::ranges::any_of(a_fragments, [&](const std::string& a_fragment)
        {
            const auto fragment = LowercaseKey(a_fragment);
            return !fragment.empty() && editorID.contains(fragment);
        });
    }

    WeatherResolutionCache& GetWeatherResolutionCache()
    {
        static WeatherResolutionCache cache;
        return cache;
    }

    void ResetWeatherResolutionCache()
    {
        GetWeatherResolutionCache() = {};
    }

    const std::unordered_map<std::string, SourceWeatherSet>& ResolveSourceWeatherSets(
        RE::TESDataHandler* a_dataHandler)
    {
        auto& cache = GetWeatherResolutionCache();
        if (!cache.initialized || cache.dataHandler != a_dataHandler)
        {
            cache = {};
            cache.dataHandler = a_dataHandler;
            cache.sourceWeatherSets = BuildSourceWeatherSets(a_dataHandler);
            std::size_t staticWeatherCount = 0;
            for (auto* weather : a_dataHandler->GetFormArray<RE::TESWeather>())
            {
                if (weather)
                {
                    cache.fxClassifications[weather->GetFormID()] = EditorIDContainsFX(weather);
                    const bool staticWeather = ComputeStaticWeather(weather);
                    cache.staticClassifications[weather->GetFormID()] = staticWeather;
                    staticWeatherCount += staticWeather ? 1 : 0;
                }
            }
            cache.initialized = true;
            DetailedLogging::Info(
                "Resolved loaded weather records for {} source plugin(s); cached FX classification for {} weather(s) and {} static weather(s)",
                cache.sourceWeatherSets.size(),
                cache.fxClassifications.size(),
                staticWeatherCount);
        }
        return cache.sourceWeatherSets;
    }

    bool SameTargetPlugins(
        const std::vector<std::string>& a_left,
        const std::vector<std::string>& a_right)
    {
        return a_left.size() == a_right.size() &&
               std::ranges::equal(a_left, a_right, [](const std::string& a_lhs, const std::string& a_rhs)
                   { return Config::IEquals(a_lhs, a_rhs); });
    }

    SourceWeatherSet ResolveProfileWeatherSet(
        RE::TESDataHandler* a_dataHandler,
        const std::vector<std::string>& a_targetPlugins,
        const std::string& a_profileName,
        bool a_excludeTargetPlugins = false)
    {
        const auto& sourceWeatherSets = ResolveSourceWeatherSets(a_dataHandler);
        auto& profileCache = GetWeatherResolutionCache().profileWeatherSets[ProfileNameFromKey(a_profileName)];
        if (profileCache.excludesTargetPlugins != a_excludeTargetPlugins ||
            !SameTargetPlugins(profileCache.targetPlugins, a_targetPlugins))
        {
            profileCache.targetPlugins = a_targetPlugins;
            profileCache.excludesTargetPlugins = a_excludeTargetPlugins;
            if (a_excludeTargetPlugins)
            {
                profileCache.weathers = BuildUnclaimedWeatherSet(a_dataHandler, sourceWeatherSets, a_targetPlugins);
                DetailedLogging::Info(
                    "Resolved catch-all TuningUtil profile {} to {} loaded weather(s), excluding weather records claimed by {} plugin(s)",
                    a_profileName,
                    profileCache.weathers.size(),
                    a_targetPlugins.size());
            }
            else
            {
                profileCache.weathers = BuildProfileWeatherSet(sourceWeatherSets, a_targetPlugins, a_profileName);
                DetailedLogging::Info(
                    "Resolved TuningUtil profile {} to {} loaded weather(s)",
                    a_profileName,
                    profileCache.weathers.size());
            }
        }
        return profileCache.weathers;
    }

    bool IsFXWeather(const RE::TESWeather* a_weather)
    {
        if (!a_weather)
        {
            return false;
        }

        auto& classifications = GetWeatherResolutionCache().fxClassifications;
        if (const auto cached = classifications.find(a_weather->GetFormID()); cached != classifications.end())
        {
            return cached->second;
        }
        return classifications.emplace(a_weather->GetFormID(), EditorIDContainsFX(a_weather)).first->second;
    }

    const CachedPointLightWeatherExclusions& ResolvePointLightWeatherExclusions(
        const std::string_view a_profileName,
        const TuningUtil::Settings& a_settings)
    {
        auto& cached = GetWeatherResolutionCache()
                           .pointLightWeatherExclusions[LowercaseKey(std::string(a_profileName))];
        if (cached.initialized &&
            cached.configuredRegions == a_settings.pointLightEffectLightingExclusions)
        {
            return cached;
        }

        cached = {};
        cached.initialized = true;
        cached.configuredRegions = a_settings.pointLightEffectLightingExclusions;
        for (const auto& configured : cached.configuredRegions)
        {
            auto* region = Config::LiteForm::FromString(configured).Get<RE::TESRegion>();
            if (!region)
            {
                logger::warn(
                    "Effect Lighting profile {} could not resolve point-light REGN {}; its WTHR records will not be excluded",
                    a_profileName,
                    configured);
                continue;
            }
            cached.regionFormIDs.insert(region->GetFormID());

            if (!region->dataList)
            {
                continue;
            }
            for (auto* data : region->dataList->regionDataList)
            {
                if (!data || data->GetType() != RE::TESRegionData::Type::kWeather)
                {
                    continue;
                }
                auto* weatherData = static_cast<RE::TESRegionDataWeather*>(data);
                for (auto* weatherType : weatherData->weatherTypes)
                {
                    auto* weather = weatherType ? weatherType->weather : nullptr;
                    if (weather)
                    {
                        cached.weatherFormIDs.insert(weather->GetFormID());
                    }
                }
            }
        }
        return cached;
    }

    bool IsStaticWeather(const RE::TESWeather* a_weather)
    {
        if (!a_weather)
        {
            return false;
        }

        auto& classifications = GetWeatherResolutionCache().staticClassifications;
        if (const auto cached = classifications.find(a_weather->GetFormID()); cached != classifications.end())
        {
            return cached->second;
        }
        return classifications.emplace(a_weather->GetFormID(), ComputeStaticWeather(a_weather)).first->second;
    }

    SourceWeatherSet FilterProfileWeathers(
        const Settings& a_settings,
        const SourceWeatherSet& a_weatherSet,
        const std::string& a_profileName)
    {
        const auto profileKey = LowercaseKey(ProfileNameFromKey(a_profileName));
        const auto resolve = [&](CachedResolvedWeatherFilter& a_cached,
                                 const TuningUtil::WeatherFilter& a_filter,
                                 const std::string_view a_name)
        {
            if (a_cached.configured == a_filter) return;
            a_cached = {};
            a_cached.configured = a_filter;
            for (const auto& configured : a_filter.formIDs)
            {
                if (configured.empty() || Config::IEquals(configured, "null")) continue;
                const auto formID = Config::LiteForm::FromString(configured).formID;
                if (formID == 0)
                {
                    DetailedLogging::Info("Weather {} {} is not loaded; ignoring it", a_name, configured);
                    continue;
                }
                if (!RE::TESForm::LookupByID<RE::TESWeather>(formID))
                {
                    logger::warn("Weather {} {} does not resolve to a weather; ignoring it", a_name, configured);
                    continue;
                }
                a_cached.formIDs.insert(formID);
            }
            DetailedLogging::Info(
                "Cached {} resolved weather {}(s) for profile {}",
                a_cached.formIDs.size(),
                a_name,
                a_profileName);
        };

        auto& cache = GetWeatherResolutionCache();
        auto& inclusions = cache.weatherInclusions[profileKey];
        auto& exclusions = cache.weatherExclusions[profileKey];
        resolve(inclusions, a_settings.weatherInclusions, "inclusion");
        resolve(exclusions, a_settings.weatherExclusions, "exclusion");

        const auto hasInclusions = !a_settings.weatherInclusions.formIDs.empty() ||
                                   !a_settings.weatherInclusions.contains.empty();
        SourceWeatherSet result;
        result.reserve(a_weatherSet.size());
        std::size_t notIncluded = 0;
        std::size_t excluded = 0;
        for (auto* weather : a_weatherSet)
        {
            if (!weather) continue;
            const auto included = inclusions.formIDs.contains(weather->GetFormID()) ||
                                  EditorIDContainsAny(weather, a_settings.weatherInclusions.contains);
            if (hasInclusions && !included)
            {
                ++notIncluded;
                continue;
            }
            if (exclusions.formIDs.contains(weather->GetFormID()) ||
                EditorIDContainsAny(weather, a_settings.weatherExclusions.contains))
            {
                ++excluded;
                continue;
            }
            result.push_back(weather);
        }
        if (notIncluded > 0 || excluded > 0)
        {
            DetailedLogging::Info(
                "TuningUtil profile {} omitted {} weather(s) outside its weather inclusion filter and excluded {} weather record(s)",
                a_profileName,
                notIncluded,
                excluded);
        }
        return result;
    }

    SourceWeatherSet FilterFXWeathers(
        const SourceWeatherSet& a_weatherSet,
        const std::string& a_profileName)
    {
        SourceWeatherSet result;
        result.reserve(a_weatherSet.size());
        std::size_t excluded = 0;
        for (auto* weather : a_weatherSet)
        {
            if (weather && !IsFXWeather(weather))
            {
                result.push_back(weather);
            }
            else if (weather)
            {
                ++excluded;
            }
        }
        if (excluded > 0 &&
            GetWeatherResolutionCache()
                .loggedFXFilterProfiles
                .insert(LowercaseKey(ProfileNameFromKey(a_profileName)))
                .second)
        {
            DetailedLogging::Info(
                "TuningUtil weather profile {} excluded {} FX weather record(s); FX weather tuning is Lighting-only",
                a_profileName,
                excluded);
        }
        return result;
    }

    std::unordered_set<RE::FormID> ResolveFilteredRuleFormIDs(
        const std::span<const std::string> a_configured,
        const std::string_view a_profileName,
        const std::string_view a_ruleID,
        const std::string_view a_filterName)
    {
        std::unordered_set<RE::FormID> result;
        for (const auto& configured : a_configured)
        {
            if (configured.empty() || Config::IEquals(configured, "null"))
            {
                continue;
            }
            const auto formID = Config::LiteForm::FromString(configured).formID;
            if (formID == 0)
            {
                DetailedLogging::Info(
                    "Filtered weather rule {} for profile {} has an unloaded {} FormID {}; ignoring it",
                    a_ruleID,
                    a_profileName,
                    a_filterName,
                    configured);
                continue;
            }
            if (!RE::TESForm::LookupByID<RE::TESWeather>(formID))
            {
                logger::warn(
                    "Filtered weather rule {} for profile {} has a {} FormID {} that is not a weather; ignoring it",
                    a_ruleID,
                    a_profileName,
                    a_filterName,
                    configured);
                continue;
            }
            result.insert(formID);
        }
        return result;
    }

    const CachedFilteredRuleForms& ResolveFilteredRuleForms(
        const std::string_view a_profileName,
        const TuningUtil::FilteredWeatherRule& a_rule)
    {
        const auto key = LowercaseKey(std::string(a_profileName)) + '\x1F' + LowercaseKey(a_rule.id);
        auto& cached = GetWeatherResolutionCache().filteredRuleForms[key];
        if (cached.include != a_rule.include || cached.exclude != a_rule.exclude)
        {
            cached = {};
            cached.include = a_rule.include;
            cached.exclude = a_rule.exclude;
            cached.includedFormIDs = ResolveFilteredRuleFormIDs(
                a_rule.include.formIDs,
                a_profileName,
                a_rule.id,
                "include");
            cached.excludedFormIDs = ResolveFilteredRuleFormIDs(
                a_rule.exclude.formIDs,
                a_profileName,
                a_rule.id,
                "exclude");
        }
        return cached;
    }

    bool MatchesFilteredWeatherRule(
        const RE::TESWeather* a_weather,
        const TuningUtil::FilteredWeatherRule& a_rule,
        const CachedFilteredRuleForms& a_forms,
        const bool a_globallyIncluded)
    {
        if (!a_weather)
        {
            return false;
        }
        const auto formID = a_weather->GetFormID();
        if (a_forms.excludedFormIDs.contains(formID) || EditorIDContainsAny(a_weather, a_rule.exclude.contains))
        {
            return false;
        }

        const bool hasInclusions = !a_rule.include.formIDs.empty() || !a_rule.include.contains.empty();
        const bool locallyIncluded = a_forms.includedFormIDs.contains(formID) ||
                                     EditorIDContainsAny(a_weather, a_rule.include.contains);
        if (hasInclusions)
        {
            return locallyIncluded;
        }
        return a_globallyIncluded;
    }

    std::size_t ApplySettingsToWeatherSet(
        const Settings& a_settings,
        const SourceWeatherSet& a_weatherSet,
        const AnchorValues& a_anchors,
        const std::span<const std::string> a_profiles)
    {
        std::size_t patched = 0;
        const auto brightness = ResolveBrightnessWithLinks(a_settings.brightnessMultiplier, a_settings.links.weather);
        const auto compression = ResolveCompressionWithLinks(a_settings.betweenWeatherCompression, a_settings.links.weather);
        const auto withinWeatherCompression = ResolveWithinWeatherCompressionWithLinks(
            a_settings.withinWeatherCompression,
            a_settings.links.weather);
        const auto saturation = ResolveSaturation(a_settings.saturationMultiplier, a_settings.links.weather);
        const auto hueScales = ResolveHueScales(a_settings.hueScales);
        const auto hueShift = ResolveHueShift(a_settings.hueShift, a_settings.links.weather);
        const bool brightnessActive = BrightnessIsActive(brightness.values);
        const bool compressionActive = CompressionIsActive(compression.values);
        const bool withinWeatherCompressionActive = CompressionIsActive(withinWeatherCompression.values);
        const bool dynamicAmbientActive =
            a_settings.dynamicAmbientWithin.darkLimit ||
            a_settings.dynamicAmbientWithin.brightLimit ||
            a_settings.dynamicAmbientBetween.darkLimit ||
            a_settings.dynamicAmbientBetween.brightLimit;
        const bool dynamicSunlightActive =
            a_settings.dynamicSunlightWithin.darkLimit ||
            a_settings.dynamicSunlightWithin.brightLimit ||
            a_settings.dynamicSunlightBetween.darkLimit ||
            a_settings.dynamicSunlightBetween.brightLimit;
        const bool saturationActive = SaturationIsActive(saturation) || HueScalesAreActive(hueScales);
        const bool hueShiftActive = HueShiftIsActive(hueShift);
        const bool weatherColorActive =
            brightnessActive ||
            compressionActive ||
            withinWeatherCompressionActive ||
            dynamicAmbientActive ||
            dynamicSunlightActive ||
            saturationActive ||
            hueShiftActive;

        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }

            CaptureBaselineIfNeeded(weather);
            RestoreBaseline(weather);
        }

        const auto changedVolumetricLighting = ApplyVolumetricLightingSettings(
            a_weatherSet,
            a_settings.volumetricLightingIntensityMultiplier,
            saturation,
            hueScales,
            hueShift,
            a_settings.hueRanges);
        if (!changedVolumetricLighting.empty())
        {
            DetailedLogging::Info(
                "Applied volumetric-lighting intensity {:.4f}x and color saturation settings to {} unique VOLI record(s)",
                std::max(0.0, a_settings.volumetricLightingIntensityMultiplier),
                changedVolumetricLighting.size());
        }
        if (compressionActive)
        {
            for (auto* weather : a_weatherSet)
            {
                if (weather)
                {
                    ApplyCompression(weather, compression, a_anchors);
                }
            }
        }

        if (withinWeatherCompressionActive)
        {
            for (auto* weather : a_weatherSet)
            {
                if (weather && !IsStaticWeather(weather))
                {
                    ApplyWithinWeatherCompression(weather, withinWeatherCompression);
                }
            }
        }

        if (brightnessActive)
        {
            ApplyBrightness(a_weatherSet, brightness);
        }

        DynamicBrightnessStatus dynamicAmbientWithinStatus;
        DynamicBrightnessStatus dynamicAmbientBetweenStatus;
        DynamicBrightnessStatus dynamicSunlightWithinStatus;
        DynamicBrightnessStatus dynamicSunlightBetweenStatus;
        ApplyDynamicBrightnessWithin(
            a_weatherSet,
            a_settings.dynamicAmbientWithin,
            DynamicBrightnessField::ambient,
            brightness,
            dynamicAmbientWithinStatus);
        ApplyDynamicBrightnessBetween(
            a_weatherSet,
            a_settings.dynamicAmbientBetween,
            DynamicBrightnessField::ambient,
            brightness,
            dynamicAmbientBetweenStatus);
        ApplyDynamicBrightnessWithin(
            a_weatherSet,
            a_settings.dynamicSunlightWithin,
            DynamicBrightnessField::sunlight,
            brightness,
            dynamicSunlightWithinStatus);
        ApplyDynamicBrightnessBetween(
            a_weatherSet,
            a_settings.dynamicSunlightBetween,
            DynamicBrightnessField::sunlight,
            brightness,
            dynamicSunlightBetweenStatus);
        CacheDynamicBrightnessStatus(
            a_profiles,
            DynamicAmbientMode::within,
            DynamicBrightnessField::ambient,
            dynamicAmbientWithinStatus);
        CacheDynamicBrightnessStatus(
            a_profiles,
            DynamicAmbientMode::between,
            DynamicBrightnessField::ambient,
            dynamicAmbientBetweenStatus);
        CacheDynamicBrightnessStatus(
            a_profiles,
            DynamicAmbientMode::within,
            DynamicBrightnessField::sunlight,
            dynamicSunlightWithinStatus);
        CacheDynamicBrightnessStatus(
            a_profiles,
            DynamicAmbientMode::between,
            DynamicBrightnessField::sunlight,
            dynamicSunlightBetweenStatus);

        if (!weatherColorActive && changedVolumetricLighting.empty())
        {
            return 0;
        }

        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }

            const bool volumetricLightingChanged = ReferencesChangedVolumetricLighting(weather, changedVolumetricLighting);
            if (!weatherColorActive && !volumetricLightingChanged)
            {
                continue;
            }

            if (saturationActive)
            {
                ApplySaturation(
                    weather,
                    saturation,
                    hueScales,
                    a_settings.hueRanges);
            }
            if (hueShiftActive)
            {
                ApplyHueShift(
                    weather,
                    hueShift,
                    a_settings.hueRanges);
            }

            ++patched;
        }

        return patched;
    }

    struct ActiveFilteredWeatherProfile
    {
        std::string name;
        Settings settings;
        ProfilePluginTargets plugins;
        SourceWeatherSet targets;
    };

    struct FilteredHueComponent
    {
        HueShiftBands shift;
        HueRanges ranges;
    };

    struct FilteredSaturationComponent
    {
        double multiplier = 1.0;
        AmbientHueScaleValues hueScales{ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
        HueRanges ranges;
    };

    struct FilteredColorAdjustment
    {
        double brightness = 1.0;
        std::vector<FilteredSaturationComponent> saturation;
        std::vector<FilteredHueComponent> hue;

        bool Active() const
        {
            return std::abs(brightness - 1.0) > 0.0001 ||
                   !saturation.empty() ||
                   !hue.empty();
        }
    };

    constexpr std::size_t kFilteredWeatherFieldCount = kBrightnessFieldCount + 1;
    using FilteredWeatherAdjustments =
        std::array<std::array<FilteredColorAdjustment, RE::TESWeather::ColorTime::kTotal>, kFilteredWeatherFieldCount>;

    std::optional<std::size_t> FilteredWeatherFieldIndex(const std::string_view a_target)
    {
        const auto target = LowercaseKey(std::string(a_target));
        const auto found = std::ranges::find_if(kBrightnessFieldNames, [&](const auto a_name)
            { return LowercaseKey(std::string(a_name)) == target; });
        if (found == kBrightnessFieldNames.end() && target == "volumetriclighting")
        {
            return kBrightnessFieldCount;
        }
        return found != kBrightnessFieldNames.end() ?
                   std::optional<std::size_t>{ static_cast<std::size_t>(std::distance(kBrightnessFieldNames.begin(), found)) } :
                   std::nullopt;
    }

    double FilteredAdjustmentValue(
        const Settings& a_settings,
        const TuningUtil::FilteredWeatherRule& a_rule)
    {
        if (const auto exact = a_settings.filteredWeatherAdjustments.find(a_rule.id);
            exact != a_settings.filteredWeatherAdjustments.end())
        {
            return exact->second;
        }
        const auto insensitive = std::ranges::find_if(
            a_settings.filteredWeatherAdjustments,
            [&](const auto& a_entry) { return Config::IEquals(a_entry.first, a_rule.id); });
        return insensitive != a_settings.filteredWeatherAdjustments.end() ? insensitive->second : a_rule.defaultValue;
    }

    double ConstrainFilteredMasterGain(
        const SourceWeatherSet& a_weatherSet,
        const std::size_t a_field,
        const std::array<bool, RE::TESWeather::ColorTime::kTotal>& a_times,
        const double a_requestedGain)
    {
        const double requestedGain = std::max(0.1, a_requestedGain);
        if (std::abs(requestedGain - 1.0) <= 0.0001)
        {
            return 1.0;
        }

        double maximumValue = 0.0;
        double minimumFloorValue = 256.0;
        for (auto* weather : a_weatherSet)
        {
            if (!weather)
            {
                continue;
            }
            for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
            {
                if (!a_times[time])
                {
                    continue;
                }
                ForEachFieldColor(weather, a_field, time, [&](const RE::Color& a_color)
                {
                    const double value = HSVValue(a_color);
                    maximumValue = std::max(maximumValue, value);
                    if (value >= 10.0) minimumFloorValue = std::min(minimumFloorValue, value);
                });
            }
        }
        if (maximumValue <= 0.0)
        {
            return 1.0;
        }
        if (requestedGain > 1.0)
        {
            return std::min(requestedGain, 255.0 / maximumValue);
        }
        return minimumFloorValue <= 255.0 ? std::max(requestedGain, 1.0 / minimumFloorValue) : requestedGain;
    }

    HueShiftBands FilteredHueShift(
        const TuningUtil::FilteredWeatherSetting& a_setting,
        const double a_value)
    {
        HueShiftBands result;
        if (!a_setting.hue)
        {
            result.red = result.orange = result.yellow = result.green = result.teal = result.blue = result.magenta = a_value;
        }
        else if (*a_setting.hue == "red") result.red = a_value;
        else if (*a_setting.hue == "orange") result.orange = a_value;
        else if (*a_setting.hue == "yellow") result.yellow = a_value;
        else if (*a_setting.hue == "green") result.green = a_value;
        else if (*a_setting.hue == "teal") result.teal = a_value;
        else if (*a_setting.hue == "blue") result.blue = a_value;
        else if (*a_setting.hue == "magenta") result.magenta = a_value;
        return result;
    }

    HueShiftBands FilteredLinkedHueShift(
        const TuningUtil::FilteredWeatherSetting& a_setting,
        const AmbientHueScaleValues& a_source,
        const double a_scale)
    {
        HueShiftBands result{
            a_source.red * a_scale,
            a_source.orange * a_scale,
            a_source.yellow * a_scale,
            a_source.green * a_scale,
            a_source.teal * a_scale,
            a_source.blue * a_scale,
            a_source.magenta * a_scale,
        };
        if (!a_setting.hue) return result;
        const auto selected = *a_setting.hue;
        if (selected != "red") result.red = 0.0;
        if (selected != "orange") result.orange = 0.0;
        if (selected != "yellow") result.yellow = 0.0;
        if (selected != "green") result.green = 0.0;
        if (selected != "teal") result.teal = 0.0;
        if (selected != "blue") result.blue = 0.0;
        if (selected != "magenta") result.magenta = 0.0;
        return result;
    }

    double ScaledFilteredValue(
        const TuningUtil::FilteredWeatherOperation a_operation,
        const double a_value,
        const double a_scale)
    {
        return a_operation == TuningUtil::FilteredWeatherOperation::hueShift ?
                   a_value * a_scale :
                   1.0 + ((a_value - 1.0) * a_scale);
    }

    struct FilteredRuleResolution
    {
        TuningUtil::FilteredWeatherOperation operation;
        std::array<bool, kFilteredWeatherFieldCount> active{};
        std::array<double, kFilteredWeatherFieldCount> values{};
        std::array<HueShiftBands, kFilteredWeatherFieldCount> hue{};
    };

    void AddHueShift(HueShiftBands& a_target, const HueShiftBands& a_value)
    {
        a_target.red += a_value.red;
        a_target.orange += a_value.orange;
        a_target.yellow += a_value.yellow;
        a_target.green += a_value.green;
        a_target.teal += a_value.teal;
        a_target.blue += a_value.blue;
        a_target.magenta += a_value.magenta;
    }

    HueShiftBands ScaledHueShift(HueShiftBands a_value, const double a_scale)
    {
        a_value.red *= a_scale;
        a_value.orange *= a_scale;
        a_value.yellow *= a_scale;
        a_value.green *= a_scale;
        a_value.teal *= a_scale;
        a_value.blue *= a_scale;
        a_value.magenta *= a_scale;
        return a_value;
    }

    FilteredRuleResolution ResolveFilteredRule(
        const Settings& a_settings,
        const TuningUtil::FilteredWeatherRule& a_rule,
        const double a_value,
        const SourceWeatherSet& a_matching)
    {
        FilteredRuleResolution result{ .operation = a_rule.settings.front().operation };
        result.values.fill(1.0);

        std::array<std::optional<SettingLinkResolution>, kFilteredWeatherFieldCount> links{};
        std::array<double, kFilteredWeatherFieldCount> baseValues{};
        baseValues.fill(1.0);
        std::array<AmbientHueScaleValues, kFilteredWeatherFieldCount> baseHueShifts{};
        if (result.operation == TuningUtil::FilteredWeatherOperation::brightness)
        {
            const auto resolved = ResolveBrightnessWithLinks(a_settings.brightnessMultiplier, a_settings.links.weather);
            std::ranges::copy(resolved.links, links.begin());
            const auto values = BaseBrightnessMultipliers(resolved.values);
            std::ranges::copy(values, baseValues.begin());
        }
        else if (result.operation == TuningUtil::FilteredWeatherOperation::saturation)
        {
            const auto resolved = ResolveSaturation(a_settings.saturationMultiplier, a_settings.links.weather);
            links = resolved.links;
            const auto values = SaturationMultipliers(resolved.values);
            std::ranges::copy(values, baseValues.begin());
        }
        else
        {
            const auto resolved = ResolveHueShift(a_settings.hueShift, a_settings.links.weather);
            links = resolved.links;
            std::ranges::copy(resolved.values, baseHueShifts.begin());
        }

        const auto localLink = a_rule.localLink ? FilteredWeatherFieldIndex(*a_rule.localLink) : std::nullopt;

        for (const auto& setting : a_rule.settings)
        {
            const auto field = FilteredWeatherFieldIndex(setting.target);
            if (!field) continue;
            if (!localLink && links[*field] && !setting.ignoreLink) continue;
            if (result.operation == TuningUtil::FilteredWeatherOperation::hueShift)
            {
                const auto shift = localLink ?
                                       FilteredLinkedHueShift(setting, baseHueShifts[*localLink], a_value * setting.scale) :
                                       FilteredHueShift(setting, ScaledFilteredValue(result.operation, a_value, setting.scale));
                if (!HueShiftBandsAreActive(shift)) continue;
                result.active[*field] = true;
                AddHueShift(result.hue[*field], shift);
            }
            else
            {
                const auto localValue = localLink ?
                                            1.0 + ((baseValues[*localLink] - 1.0) * a_value) :
                                            a_value;
                const auto value = ScaledFilteredValue(result.operation, localValue, setting.scale);
                if (std::abs(value - 1.0) <= 0.0001) continue;
                result.active[*field] = true;
                result.values[*field] = result.operation == TuningUtil::FilteredWeatherOperation::brightness ?
                                            ConstrainFilteredMasterGain(
                                                a_matching,
                                                *field,
                                                a_rule.times,
                                                value) :
                                            std::max(0.0, value);
            }
        }

        std::array<std::uint8_t, kFilteredWeatherFieldCount> states{};
        std::function<bool(std::size_t)> resolve = [&](const std::size_t a_field)
        {
            if (states[a_field] == 2) return result.active[a_field];
            if (states[a_field] == 1) return false;
            states[a_field] = 1;
            if (!result.active[a_field] && links[a_field] && resolve(links[a_field]->index))
            {
                result.active[a_field] = true;
                if (result.operation == TuningUtil::FilteredWeatherOperation::hueShift)
                {
                    result.hue[a_field] = ScaledHueShift(result.hue[links[a_field]->index], links[a_field]->scale);
                }
                else
                {
                    result.values[a_field] = 1.0 + ((result.values[links[a_field]->index] - 1.0) * links[a_field]->scale);
                }
            }
            states[a_field] = 2;
            return result.active[a_field];
        };
        for (std::size_t field = 0; field < result.active.size(); ++field) resolve(field);
        return result;
    }

    bool FilteredModuleRuleIsLinked(
        const Settings& a_settings,
        const TuningUtil::FilteredWeatherRule& a_rule)
    {
        if (Config::IEquals(a_rule.id, a_rule.controlID) || a_rule.settings.size() != 1)
        {
            return false;
        }
        if (a_rule.localLink) return false;
        const auto field = FilteredWeatherFieldIndex(a_rule.settings.front().target);
        if (!field)
        {
            return false;
        }
        switch (a_rule.settings.front().operation)
        {
        case TuningUtil::FilteredWeatherOperation::brightness:
        {
            const auto links = ResolveBrightnessWithLinks(a_settings.brightnessMultiplier, a_settings.links.weather).links;
            return *field < links.size() && links[*field].has_value();
        }
        case TuningUtil::FilteredWeatherOperation::saturation:
            return ResolveSaturation(a_settings.saturationMultiplier, a_settings.links.weather).links[*field].has_value();
        case TuningUtil::FilteredWeatherOperation::hueShift:
            return ResolveHueShift(a_settings.hueShift, a_settings.links.weather).links[*field].has_value();
        }
        return false;
    }

    std::size_t ApplyFilteredWeatherSettings(
        const std::span<const ActiveFilteredWeatherProfile> a_profiles)
    {
        std::unordered_map<RE::TESWeather*, FilteredWeatherAdjustments> adjustments;
        std::unordered_map<RE::BGSVolumetricLighting*, FilteredColorAdjustment> volumetricAdjustments;
        std::size_t activeRules = 0;

        for (const auto& profile : a_profiles)
        {
            const auto globallyIncluded = FilterProfileWeathers(profile.settings, profile.targets, profile.name);
            std::unordered_set<RE::FormID> globallyIncludedFormIDs;
            for (const auto* weather : globallyIncluded)
            {
                if (weather) globallyIncludedFormIDs.insert(weather->GetFormID());
            }
            for (const auto& rule : TuningUtil::GetFilteredWeatherRules(profile.name))
            {
                if (rule.settings.empty() || FilteredModuleRuleIsLinked(profile.settings, rule)) continue;
                const double value = FilteredAdjustmentValue(profile.settings, rule);
                const auto operation = rule.settings.front().operation;
                const double neutral = rule.localLink ? 0.0 :
                                           operation == TuningUtil::FilteredWeatherOperation::hueShift ? 0.0 : 1.0;
                if (std::abs(value - neutral) <= 0.0001)
                {
                    continue;
                }

                const auto& forms = ResolveFilteredRuleForms(profile.name, rule);
                SourceWeatherSet matching;
                const bool timeSpecific = !std::ranges::all_of(rule.times, std::identity{});
                for (auto* weather : profile.targets)
                {
                    if ((!timeSpecific || !IsStaticWeather(weather)) && MatchesFilteredWeatherRule(
                            weather,
                            rule,
                            forms,
                            weather && globallyIncludedFormIDs.contains(weather->GetFormID())))
                    {
                        matching.push_back(weather);
                    }
                }
                if (matching.empty())
                {
                    continue;
                }

                const auto resolved = ResolveFilteredRule(
                    profile.settings,
                    rule,
                    value,
                    matching);
                std::unordered_set<RE::BGSVolumetricLighting*> ruleVolumetricLighting;
                const auto accumulate = [&](FilteredColorAdjustment& a_adjustment, const std::size_t a_field)
                {
                    if (operation == TuningUtil::FilteredWeatherOperation::brightness)
                        a_adjustment.brightness *= resolved.values[a_field];
                    else if (operation == TuningUtil::FilteredWeatherOperation::saturation)
                        a_adjustment.saturation.push_back({
                            resolved.values[a_field],
                            rule.hueScales ? ResolveHueScales(*rule.hueScales) :
                                             AmbientHueScaleValues{ 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 },
                            profile.settings.hueRanges,
                        });
                    else
                        a_adjustment.hue.push_back({ resolved.hue[a_field], profile.settings.hueRanges });
                };
                for (auto* weather : matching)
                {
                    auto& weatherAdjustments = adjustments[weather];
                    for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
                    {
                        if (!rule.times[time])
                        {
                            continue;
                        }
                        for (std::size_t field = 0; field < resolved.active.size(); ++field)
                        {
                            if (!resolved.active[field]) continue;
                            if (field < kBrightnessFieldCount)
                            {
                                accumulate(weatherAdjustments[field][time], field);
                            }
                            else if (auto* volumetricLighting = weather->volumetricLighting[time];
                                     volumetricLighting && ruleVolumetricLighting.insert(volumetricLighting).second)
                            {
                                accumulate(volumetricAdjustments[volumetricLighting], field);
                            }
                        }
                    }
                }
                ++activeRules;
            }
        }

        std::size_t patched = 0;
        for (auto& [weather, weatherAdjustments] : adjustments)
        {
            if (!weather)
            {
                continue;
            }
            CaptureBaselineIfNeeded(weather);
            auto weatherChanged = false;
            for (std::size_t field = 0; field < kBrightnessFieldCount; ++field)
            {
                for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
                {
                    const auto& adjustment = weatherAdjustments[field][time];
                    if (!adjustment.Active())
                    {
                        continue;
                    }
                    ForEachFieldColor(weather, field, time, [&](RE::Color& a_color)
                    {
                        if (std::abs(adjustment.brightness - 1.0) > 0.0001)
                        {
                            MultiplyBrightnessColor(a_color, adjustment.brightness);
                        }
                        if (!adjustment.saturation.empty())
                        {
                            double multiplier = 1.0;
                            for (const auto& component : adjustment.saturation)
                                multiplier *= 1.0 + ((component.multiplier - 1.0) *
                                                     ColorHueScale(a_color, component.hueScales, component.ranges));
                            SaturateColor(a_color, multiplier);
                        }
                        double hueShift = 0.0;
                        for (const auto& component : adjustment.hue)
                        {
                            hueShift += ColorHueShiftDegrees(a_color, component.shift, component.ranges);
                        }
                        ShiftHue(a_color, hueShift);
                    });
                    weatherChanged = true;
                }
            }
            patched += weatherChanged ? 1 : 0;
        }

        std::size_t volumetricPatched = 0;
        auto* stat = MPL::Config::StatData::GetSingleton();
        for (auto& [volumetricLighting, adjustment] : volumetricAdjustments)
        {
            if (!volumetricLighting || !adjustment.Active()) continue;
            stat->volumetricLightingColorBaselines.try_emplace(volumetricLighting, volumetricLighting->color);
            auto adjustedColor = volumetricLighting->color;
            if (!adjustment.saturation.empty())
            {
                double multiplier = 1.0;
                for (const auto& component : adjustment.saturation)
                    multiplier *= 1.0 + ((component.multiplier - 1.0) *
                                         ColorHueScale(adjustedColor, component.hueScales, component.ranges));
                SaturateColor(adjustedColor, multiplier);
            }
            double hueShift = 0.0;
            for (const auto& component : adjustment.hue)
                hueShift += ColorHueShiftDegrees(adjustedColor, component.shift, component.ranges);
            ShiftHue(adjustedColor, hueShift);
            volumetricLighting->color = adjustedColor;
            ++volumetricPatched;
        }

        if (activeRules > 0)
        {
            DetailedLogging::Info(
                "Applied {} active filtered slider rule(s) to {} weather and {} volumetric-lighting record(s)",
                activeRules,
                patched,
                volumetricPatched);
        }
        return patched + volumetricPatched;
    }

    std::size_t ApplyFXEffectLightingSettings(RE::TESDataHandler* a_dataHandler)
    {
        struct ActiveProfile
        {
            std::string name;
            RecordFilter::Resolved filter;
            std::unordered_set<RE::FormID> pointLightRegionExclusions;
            std::unordered_set<RE::FormID> pointLightWeatherExclusions;
        };

        std::vector<ActiveProfile> profiles;
        for (auto& profileName : GetOrderedFXEffectLightingProfiles())
        {
            auto settings = LoadSettings(profileName);
            if (settings && settings->EnableProfile)
            {
                const auto& pointLightExclusions =
                    ResolvePointLightWeatherExclusions(profileName, *settings);
                profiles.push_back({
                    profileName,
                    RecordFilter::Resolve(
                        settings->effectLightingInclusions,
                        settings->effectLightingExclusions,
                        settings->effectLightingPluginInclusions,
                        settings->effectLightingPluginExclusions),
                    pointLightExclusions.regionFormIDs,
                    pointLightExclusions.weatherFormIDs,
                });
            }
        }
        if (profiles.empty())
        {
            return 0;
        }

        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>> runtimeEmittanceSources;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        auto* loadedData = cell ? cell->GetRuntimeData().loadedData : nullptr;
        if (loadedData)
        {
            for (const auto& entry : loadedData->emittanceSourceRefMap)
            {
                auto* source = entry.first;
                if (!source || !source->Is(RE::FormType::Region))
                {
                    continue;
                }
                auto* region = static_cast<RE::TESRegion*>(source);
                if (!region->currentWeather)
                {
                    continue;
                }
                runtimeEmittanceSources[region->currentWeather->GetFormID()].insert(region->GetFormID());
            }
        }

        std::unordered_map<std::string, std::vector<RE::TESWeather*>> weatherGroups;
        std::unordered_map<std::string, std::vector<std::string>> groupProfiles;
        std::unordered_map<std::string, std::size_t> profileTargetCounts;
        std::unordered_map<std::string, std::size_t> profileWeatherExclusionCounts;
        for (auto* weather : a_dataHandler->GetFormArray<RE::TESWeather>())
        {
            const auto runtimeSources = weather ? runtimeEmittanceSources.find(weather->GetFormID()) : runtimeEmittanceSources.end();
            if (!weather || (!IsFXWeather(weather) && runtimeSources == runtimeEmittanceSources.end()))
            {
                continue;
            }

            std::string signature;
            std::vector<std::string> matchingProfiles;
            for (const auto& profile : profiles)
            {
                if (!RecordFilter::Matches(weather, profile.filter))
                {
                    continue;
                }
                bool pointLightWeather = profile.pointLightWeatherExclusions.contains(weather->GetFormID());
                if (!pointLightWeather && runtimeSources != runtimeEmittanceSources.end())
                {
                    pointLightWeather = std::ranges::any_of(
                        runtimeSources->second,
                        [&](const RE::FormID a_region)
                        {
                            return profile.pointLightRegionExclusions.contains(a_region);
                        });
                }
                if (pointLightWeather)
                {
                    ++profileWeatherExclusionCounts[profile.name];
                    continue;
                }
                matchingProfiles.push_back(profile.name);
                signature.append(profile.name).push_back('\x1F');
                ++profileTargetCounts[profile.name];
            }
            if (!matchingProfiles.empty())
            {
                weatherGroups[signature].push_back(weather);
                groupProfiles.try_emplace(signature, std::move(matchingProfiles));
            }
        }

        std::size_t patched = 0;
        for (const auto& [signature, weathers] : weatherGroups)
        {
            const auto settings = TuningUtil::ResolveSettingsStack(groupProfiles[signature]);
            const auto& effect = settings.fxEffectLighting;
            const auto brightnessActive = std::abs(effect.brightnessMultiplier - 1.0) > 0.0001;
            const auto saturationActive = std::abs(effect.saturationMultiplier - 1.0) > 0.0001;
            const auto hueShiftActive = std::abs(effect.hueShift.red) > 0.0001 ||
                                        std::abs(effect.hueShift.orange) > 0.0001 ||
                                        std::abs(effect.hueShift.yellow) > 0.0001 ||
                                        std::abs(effect.hueShift.green) > 0.0001 ||
                                        std::abs(effect.hueShift.teal) > 0.0001 ||
                                        std::abs(effect.hueShift.blue) > 0.0001 ||
                                        std::abs(effect.hueShift.magenta) > 0.0001;
            if (!brightnessActive && !saturationActive && !hueShiftActive)
            {
                continue;
            }
            for (auto* weather : weathers)
            {
                CaptureBaselineIfNeeded(weather);
                for (std::uint32_t time = 0; time < RE::TESWeather::ColorTime::kTotal; ++time)
                {
                    for (const auto colorType : {
                             RE::TESWeather::ColorType::kEffectLighting,
                             RE::TESWeather::ColorType::kSunlight,
                         })
                    {
                        auto& color = weather->colorData[colorType][time];
                        if (brightnessActive) MultiplyBrightnessColor(color, effect.brightnessMultiplier);
                        if (saturationActive) SaturateColor(color, effect.saturationMultiplier);
                        if (hueShiftActive)
                        {
                            ShiftHue(color, ColorHueShiftDegrees(color, effect.hueShift, settings.intHueRanges));
                        }
                    }
                }

                ++patched;
            }
        }

        for (const auto& profile : profiles)
        {
            DetailedLogging::Info(
                "Effect Lighting filter for profile {} matched {} emittance weather record(s) and excluded {} point-light weather record(s)",
                profile.name,
                profileTargetCounts[profile.name],
                profileWeatherExclusionCounts[profile.name]);
        }
        DetailedLogging::Info(
            "Applied {} effect-lighting profile stack(s) to {} emittance weather(s)",
            weatherGroups.size(),
            patched);
        return patched;
    }

    void ApplyAllSettings()
    {
        struct ActiveProfile
        {
            std::string name;
            Settings settings;
            ProfilePluginTargets plugins;
            std::unordered_set<RE::TESWeather*> targets;
        };

        struct WeatherStack
        {
            std::vector<std::string> profiles;
            SourceWeatherSet weathers;
        };

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("TESDataHandler is unavailable; TuningUtil settings were not applied");
            return;
        }

        const auto& weatherSets = ResolveSourceWeatherSets(dataHandler);
        RestoreAllCapturedWeatherBaselines();
        GetDynamicBrightnessStatuses().clear();
        if (weatherSets.empty())
        {
            logger::info("No weather records found");
            return;
        }

        std::size_t configsApplied = 0;
        std::size_t weathersApplied = 0;
        std::vector<ActiveProfile> profiles;
        std::vector<ActiveFilteredWeatherProfile> filteredProfiles;
        std::vector<std::string> claimedPlugins;

        for (auto& profileName : GetOrderedSettingsProfiles())
        {
            auto settings = LoadSettings(profileName);
            if (!settings)
            {
                continue;
            }
            if (!settings->EnableProfile)
            {
                DetailedLogging::Info("Skipping disabled TuningUtil weather profile {}", profileName);
                continue;
            }

            auto pluginTargets = ResolveProfilePluginTargets(*settings, weatherSets);
            if (!pluginTargets.catchAll) AppendUniquePlugins(claimedPlugins, pluginTargets.included);
            profiles.push_back({ profileName, std::move(*settings), std::move(pluginTargets), {} });
        }

        for (const auto& profile : TuningUtil::GetProfiles())
        {
            if (profile.filteredWeatherRules.empty())
            {
                continue;
            }
            auto profileName = profile.name;
            auto settings = LoadSettings(profileName);
            if (!settings || !settings->EnableProfile)
            {
                continue;
            }
            auto pluginTargets = ResolveProfilePluginTargets(*settings, weatherSets);
            if (!pluginTargets.catchAll) AppendUniquePlugins(claimedPlugins, pluginTargets.included);
            filteredProfiles.push_back({ profile.name, std::move(*settings), std::move(pluginTargets), {} });
        }

        for (auto& profile : profiles)
        {
            auto resolutionPlugins = profile.plugins.included;
            if (profile.plugins.catchAll)
            {
                resolutionPlugins = claimedPlugins;
                AppendUniquePlugins(resolutionPlugins, profile.plugins.excluded);
            }
            auto profileWeatherSet = ResolveProfileWeatherSet(
                dataHandler,
                resolutionPlugins,
                profile.name,
                profile.plugins.catchAll);
            profileWeatherSet = RemovePluginExcludedWeathers(
                std::move(profileWeatherSet),
                weatherSets,
                profile.plugins.excluded);
            profileWeatherSet = FilterProfileWeathers(profile.settings, profileWeatherSet, profile.name);
            profileWeatherSet = FilterFXWeathers(profileWeatherSet, profile.name);
            profile.targets.insert(profileWeatherSet.begin(), profileWeatherSet.end());
        }

        for (auto& profile : filteredProfiles)
        {
            auto resolutionPlugins = profile.plugins.included;
            if (profile.plugins.catchAll)
            {
                resolutionPlugins = claimedPlugins;
                AppendUniquePlugins(resolutionPlugins, profile.plugins.excluded);
            }
            profile.targets = ResolveProfileWeatherSet(
                dataHandler,
                resolutionPlugins,
                profile.name,
                profile.plugins.catchAll);
            profile.targets = RemovePluginExcludedWeathers(
                std::move(profile.targets),
                weatherSets,
                profile.plugins.excluded);
            profile.targets = FilterFXWeathers(profile.targets, profile.name);
        }

        SourceWeatherSet targetedWeathers;
        std::unordered_set<RE::TESWeather*> uniqueTargetedWeathers;
        for (const auto& profile : profiles)
        {
            for (auto* weather : profile.targets)
            {
                if (weather && uniqueTargetedWeathers.insert(weather).second)
                {
                    targetedWeathers.push_back(weather);
                }
            }
        }

        std::vector<WeatherStack> stacks;
        std::unordered_map<std::string, std::size_t> stackIndexes;
        for (auto* weather : targetedWeathers)
        {
            std::vector<std::string> matchingProfiles;
            std::string signature;
            for (const auto& profile : profiles)
            {
                if (!profile.targets.contains(weather))
                {
                    continue;
                }
                matchingProfiles.push_back(profile.name);
                signature.append(LowercaseKey(profile.name)).push_back('\x1F');
            }
            if (matchingProfiles.empty())
            {
                continue;
            }
            const auto [entry, inserted] = stackIndexes.try_emplace(signature, stacks.size());
            if (inserted)
            {
                stacks.push_back({ std::move(matchingProfiles), {} });
            }
            stacks[entry->second].weathers.push_back(weather);
        }

        for (auto& stack : stacks)
        {
            auto settings = TuningUtil::ResolveSettingsStack(stack.profiles);
            const auto& profileLabel = stack.profiles.back();
            const auto profileAnchors = ResolveAnchors(settings.compressionAnchor, settings.links.weather);
            const auto profileWeathersApplied = ApplySettingsToWeatherSet(
                settings,
                stack.weathers,
                profileAnchors,
                stack.profiles);
            weathersApplied += profileWeathersApplied;

            ++configsApplied;
            DetailedLogging::Info(
                "Applied {} stacked TuningUtil weather profile(s), ending with {}, to {} total weather(s)",
                stack.profiles.size(),
                profileLabel,
                profileWeathersApplied);
        }

        const auto filteredWeathersApplied = ApplyFilteredWeatherSettings(filteredProfiles);
        weathersApplied += filteredWeathersApplied;
        if (filteredWeathersApplied > 0)
        {
            ++configsApplied;
        }

        const auto fxEffectLightingApplied = ApplyFXEffectLightingSettings(dataHandler);
        weathersApplied += fxEffectLightingApplied;
        if (fxEffectLightingApplied > 0)
        {
            ++configsApplied;
        }
        const bool emittanceWeatherSettingsApplied = fxEffectLightingApplied > 0;
        if (emittanceWeatherSettingsApplied || emittanceWeatherSettingsWereApplied)
        {
            WeatherRuntime::RefreshCurrentCellEmittance();
        }
        emittanceWeatherSettingsWereApplied = emittanceWeatherSettingsApplied;
        logger::info("TuningUtil applied {} config(s) to {} weather record(s)", configsApplied, weathersApplied);
    }

    void ApplyDataLoaded()
    {
        ResetWeatherResolutionCache();
        if (TuningSettings::IsTuningMenuEnabledForSession())
        {
            auto profiles = GetOrderedSettingsProfiles();
            for (auto& profile : profiles)
            {
                for (const auto& category : GetPresetCategories(profile))
                {
                    (void)GetPresets(profile, category);
                }
            }
        }
        ApplyAllSettings();
    }

    void ReleaseRuntimeState()
    {
        emittanceWeatherSettingsWereApplied = false;
        ResetWeatherResolutionCache();
        GetDynamicBrightnessStatuses().clear();
        GetPresetCatalogs() = {};
        GetActivePresetCaches() = {};
        GetPresetPreviewCaches() = {};

        auto* stat = Config::StatData::GetSingleton();
        stat->weatherBaselines = {};
        stat->volumetricLightingIntensityBaselines = {};
        stat->volumetricLightingColorBaselines = {};
    }

    SourceWeatherSet GetProfileTargetWeathers(
        std::string& a_profileName,
        const bool a_applyWeatherFilter)
    {
        if (ProfileNameFromKey(a_profileName).empty())
        {
            logger::warn("Rejected profile-weather request with an empty TuningUtil profile name");
            return {};
        }
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            logger::warn("TESDataHandler is unavailable; selectable weathers for {} could not be enumerated", a_profileName);
            return {};
        }

        const auto& weatherSets = ResolveSourceWeatherSets(dataHandler);
        const auto settings = GetOrCreateSettings(a_profileName);
        const auto pluginTargets = ResolveProfilePluginTargets(settings, weatherSets);
        std::vector<std::string> claimedPlugins;
        for (auto& profileName : GetOrderedSettingsProfiles())
        {
            const auto* profileSettings = Config::IEquals(profileName, a_profileName) ?
                                              std::addressof(settings) :
                                              nullptr;
            const auto loaded = profileSettings ? std::optional<Settings>{} : LoadSettings(profileName);
            if (!profileSettings && loaded)
            {
                profileSettings = std::addressof(*loaded);
            }
            if (profileSettings && profileSettings->EnableProfile)
            {
                const auto targets = ResolveProfilePluginTargets(*profileSettings, weatherSets);
                if (!targets.catchAll)
                {
                    AppendUniquePlugins(claimedPlugins, targets.included);
                }
            }
        }
        auto resolutionPlugins = pluginTargets.included;
        if (pluginTargets.catchAll)
        {
            resolutionPlugins = claimedPlugins;
            AppendUniquePlugins(resolutionPlugins, pluginTargets.excluded);
        }
        auto profileWeatherSet = ResolveProfileWeatherSet(
            dataHandler,
            resolutionPlugins,
            a_profileName,
            pluginTargets.catchAll);
        profileWeatherSet = RemovePluginExcludedWeathers(
            std::move(profileWeatherSet),
            weatherSets,
            pluginTargets.excluded);
        if (a_applyWeatherFilter)
        {
            profileWeatherSet = FilterProfileWeathers(settings, profileWeatherSet, a_profileName);
        }
        profileWeatherSet = FilterFXWeathers(profileWeatherSet, a_profileName);
        const auto enumerationKey =
            LowercaseKey(ProfileNameFromKey(a_profileName)) +
            (a_applyWeatherFilter ?
                    "\x1F" "selectable" :
                    "\x1F" "filterable");
        if (GetWeatherResolutionCache()
                .loggedWeatherEnumerations
                .insert(enumerationKey)
                .second)
        {
            DetailedLogging::Info(
                "Enumerated {} {} weather(s) for profile {}",
                profileWeatherSet.size(),
                a_applyWeatherFilter ? "selectable" : "filterable",
                a_profileName);
        }
        return profileWeatherSet;
    }

    SourceWeatherSet GetSelectableWeathers(std::string& a_profileName)
    {
        return GetProfileTargetWeathers(a_profileName, true);
    }

    SourceWeatherSet GetFilterableWeathers(std::string& a_profileName)
    {
        return GetProfileTargetWeathers(a_profileName, false);
    }

    bool ProfilesShareWeatherTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile)
    {
        auto leftProfile = a_leftProfile;
        auto rightProfile = a_rightProfile;
        const auto leftTargets = GetSelectableWeathers(leftProfile);
        const auto rightTargets = GetSelectableWeathers(rightProfile);
        std::unordered_set<RE::FormID> leftFormIDs;
        for (const auto* weather : leftTargets)
        {
            if (weather) leftFormIDs.insert(weather->GetFormID());
        }
        return std::ranges::any_of(rightTargets, [&](const auto* a_weather)
            { return a_weather && leftFormIDs.contains(a_weather->GetFormID()); });
    }

    bool ProfilesShareFilteredWeatherTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile,
        const std::string_view a_ruleID)
    {
        const auto targetsFor = [&](std::string a_profileName)
        {
            SourceWeatherSet result;
            const auto* rule = TuningUtil::FindFilteredWeatherRule(a_profileName, a_ruleID);
            if (!rule)
            {
                return result;
            }

            auto targets = GetProfileTargetWeathers(a_profileName, false);
            const auto& settings = GetOrCreateSettings(a_profileName);
            const auto globallyIncluded = FilterProfileWeathers(settings, targets, a_profileName);
            std::unordered_set<RE::FormID> globallyIncludedFormIDs;
            for (const auto* weather : globallyIncluded)
            {
                if (weather) globallyIncludedFormIDs.insert(weather->GetFormID());
            }

            const auto& forms = ResolveFilteredRuleForms(a_profileName, *rule);
            const bool timeSpecific = !std::ranges::all_of(rule->times, std::identity{});
            for (auto* weather : targets)
            {
                if ((!timeSpecific || !IsStaticWeather(weather)) && MatchesFilteredWeatherRule(
                        weather,
                        *rule,
                        forms,
                        weather && globallyIncludedFormIDs.contains(weather->GetFormID())))
                {
                    result.push_back(weather);
                }
            }
            return result;
        };

        const auto leftTargets = targetsFor(a_leftProfile);
        const auto rightTargets = targetsFor(a_rightProfile);
        std::unordered_set<RE::FormID> leftFormIDs;
        for (const auto* weather : leftTargets)
        {
            if (weather) leftFormIDs.insert(weather->GetFormID());
        }
        return std::ranges::any_of(rightTargets, [&](const auto* a_weather)
            { return a_weather && leftFormIDs.contains(a_weather->GetFormID()); });
    }

    DynamicAmbientRange GetDynamicAmbientRange(
        std::string& a_profileName,
        const DynamicAmbientMode a_mode,
        const DynamicBrightnessField a_field)
    {
        return AnalyzeDynamicAmbientRange(
            GetProfileTargetWeathers(a_profileName, true),
            a_mode,
            a_field);
    }

    std::optional<DynamicBrightnessStatus> GetDynamicBrightnessStatus(
        std::string& a_profileName,
        const DynamicAmbientMode a_mode,
        const DynamicBrightnessField a_field)
    {
        const auto& statuses = GetDynamicBrightnessStatuses();
        const auto found = statuses.find(DynamicBrightnessStatusKey(a_profileName, a_mode, a_field));
        return found != statuses.end() ?
                   std::optional{ found->second } :
                   std::nullopt;
    }

    SourceWeatherSet GetFXWeathers()
    {
        SourceWeatherSet result;
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler)
        {
            return result;
        }

        ResolveSourceWeatherSets(dataHandler);
        for (auto* weather : dataHandler->GetFormArray<RE::TESWeather>())
        {
            if (weather && IsFXWeather(weather))
            {
                result.push_back(weather);
            }
        }
        return result;
    }

    bool ProfilesShareEffectLightingTarget(
        const std::string& a_leftProfile,
        const std::string& a_rightProfile)
    {
        auto leftProfile = a_leftProfile;
        auto rightProfile = a_rightProfile;
        const auto& left = TuningUtil::GetSettings(leftProfile);
        const auto& right = TuningUtil::GetSettings(rightProfile);
        const auto leftFilter = RecordFilter::Resolve(
            left.effectLightingInclusions,
            left.effectLightingExclusions,
            left.effectLightingPluginInclusions,
            left.effectLightingPluginExclusions);
        const auto rightFilter = RecordFilter::Resolve(
            right.effectLightingInclusions,
            right.effectLightingExclusions,
            right.effectLightingPluginInclusions,
            right.effectLightingPluginExclusions);
        return std::ranges::any_of(GetFXWeathers(), [&](const auto* a_weather)
            { return RecordFilter::Matches(a_weather, leftFilter) && RecordFilter::Matches(a_weather, rightFilter); });
    }

    std::optional<std::string> NormalizePresetPathComponent(
        std::string a_value,
        const bool a_stripJsonExtension,
        std::string& a_error)
    {
        const auto first = a_value.find_first_not_of(" \t\r\n");
        const auto last = a_value.find_last_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            a_error = "Category and preset name are required.";
            return std::nullopt;
        }
        a_value = a_value.substr(first, last - first + 1);
        if (a_stripJsonExtension && Config::IEquals(std::filesystem::path(a_value).extension().string(), ".json"))
        {
            a_value = std::filesystem::path(a_value).stem().string();
        }
        constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
        if (a_value.empty() || a_value == "." || a_value == ".." ||
            a_value.ends_with('.') || a_value.find_first_of(invalidCharacters) != std::string::npos ||
            std::ranges::any_of(a_value, [](const unsigned char a_character) { return a_character < 32; }))
        {
            a_error = "Category and preset name must be valid Windows folder and file names.";
            return std::nullopt;
        }

        auto reservedName = LowercaseKey(std::filesystem::path(a_value).stem().string());
        const auto reserved = reservedName == "con" || reservedName == "prn" || reservedName == "aux" ||
                              reservedName == "nul" ||
                              (reservedName.size() == 4 &&
                                  (reservedName.starts_with("com") || reservedName.starts_with("lpt")) &&
                                  reservedName[3] >= '1' && reservedName[3] <= '9');
        if (reserved)
        {
            a_error = "That category or preset name is reserved by Windows.";
            return std::nullopt;
        }
        return a_value;
    }

    std::vector<std::string> GetPresetCategories(std::string& a_profileName)
    {
        const auto profileName = ProfileNameFromKey(a_profileName);
        if (profileName.empty())
        {
            logger::warn("Rejected TuningUtil preset category request with an empty profile name");
            return {};
        }

        auto& catalogs = GetPresetCatalogs();
        const auto catalogKey = LowercaseKey(profileName) + "|categories";
        if (const auto cached = catalogs.find(catalogKey); cached != catalogs.end())
        {
            return cached->second;
        }

        std::vector<std::string> categories;
        const auto directory = TuningUtil::ProfileDirectory(a_profileName);
        std::error_code error;
        for (auto iterator = std::filesystem::directory_iterator(directory, error);
            !error && iterator != std::filesystem::directory_iterator();
            iterator.increment(error))
        {
            if (!iterator->is_directory(error) || error)
            {
                continue;
            }
            const auto category = iterator->path().filename().string();
            if (!std::ranges::any_of(categories, [&](const std::string& a_existing)
                    { return Config::IEquals(a_existing, category); }))
            {
                categories.push_back(category);
            }
        }
        std::ranges::sort(categories, [](const std::string& a_left, const std::string& a_right)
            { return LowercaseKey(a_left) < LowercaseKey(a_right); });
        return catalogs.emplace(catalogKey, std::move(categories)).first->second;
    }

    std::vector<std::string> GetPresets(std::string& a_profileName, const std::string& a_category)
    {
        std::vector<std::string> presets;
        const auto profileName = ProfileNameFromKey(a_profileName);
        std::string validationError;
        const auto category = NormalizePresetPathComponent(a_category, false, validationError);
        if (profileName.empty() || !category)
        {
            logger::warn("Rejected TuningUtil preset request: {}", validationError);
            return presets;
        }
        auto& catalogs = GetPresetCatalogs();
        const auto catalogKey = LowercaseKey(profileName) + "|presets|" + LowercaseKey(*category);
        if (const auto cached = catalogs.find(catalogKey); cached != catalogs.end())
        {
            return cached->second;
        }
        const auto directory = TuningUtil::ProfileDirectory(a_profileName) / *category;
        std::error_code error;
        if (std::filesystem::is_directory(directory, error))
        {
            for (auto iterator = std::filesystem::directory_iterator(directory, error);
                !error && iterator != std::filesystem::directory_iterator();
                iterator.increment(error))
            {
                const auto& preset = *iterator;
                if (preset.is_regular_file(error) && !error && Config::IEquals(preset.path().extension().string(), ".json"))
                {
                    presets.push_back(preset.path().stem().string());
                }
            }
        }
        std::ranges::sort(presets, [](const std::string& a_left, const std::string& a_right)
            { return LowercaseKey(a_left) < LowercaseKey(a_right); });
        DetailedLogging::Info("Cached {} {} preset definition(s) for {}", presets.size(), *category, profileName);
        return catalogs.emplace(catalogKey, std::move(presets)).first->second;
    }

    static ActivePresetCache* ResolveActivePresetCache(
        std::string& a_profileName,
        std::string& a_error)
    {
        a_error.clear();
        const auto profileName = ProfileNameFromKey(a_profileName);
        if (profileName.empty())
        {
            a_error = "The profile is unavailable.";
            return nullptr;
        }

        auto& caches = GetActivePresetCaches();
        const auto cacheKey = LowercaseKey(profileName);
        if (const auto cached = caches.find(cacheKey); cached != caches.end())
        {
            return std::addressof(cached->second);
        }

        ActivePresetCache result;
        for (const auto& category : GetPresetCategories(a_profileName))
        {
            std::optional<std::string> selected;
            for (const auto& preset : GetPresets(a_profileName, category))
            {
                const auto path = TuningUtil::ProfileDirectory(a_profileName) / category / (preset + ".json");
                const auto text = ReadJsonText(path);
                if (!text || !JsonOverlay::BooleanMember(*text, "autoLoadOnStartup").value_or(false))
                {
                    continue;
                }
                if (selected)
                {
                    logger::warn(
                        "TuningUtil profile {} has multiple auto-load presets in category {}; using {} and ignoring {}",
                        profileName,
                        category,
                        *selected,
                        preset);
                    continue;
                }

                selected = preset;
                const std::vector<std::string> metadataPath{ "autoLoadOnStartup" };
                const auto settings = JsonOverlay::RemovePaths(*text, metadataPath, a_error);
                const auto merged = settings ? JsonOverlay::Overlay(result.settings, *settings, a_error) : std::nullopt;
                if (!merged)
                {
                    logger::warn("Could not load auto-load preset {}: {}", path.string(), a_error);
                    return nullptr;
                }
                result.settings = std::move(*merged);
                result.presets.push_back({ category, preset });
            }
        }

        return std::addressof(caches.emplace(cacheKey, std::move(result)).first->second);
    }

    std::vector<ActivePreset> GetActivePresets(std::string& a_profileName, std::string& a_error)
    {
        const auto cacheKey = LowercaseKey(ProfileNameFromKey(a_profileName));
        auto& previews = GetPresetPreviewCaches();
        if (const auto preview = previews.find(cacheKey); preview != previews.end())
        {
            a_error.clear();
            return preview->second.presets;
        }
        const auto* active = ResolveActivePresetCache(a_profileName, a_error);
        return active ? active->presets : std::vector<ActivePreset>{};
    }

    std::optional<std::string> GetActivePresetSettings(std::string& a_profileName, std::string& a_error)
    {
        const auto* active = ResolveActivePresetCache(a_profileName, a_error);
        return active ? std::optional<std::string>{ active->settings } : std::nullopt;
    }

    void DiscardPresetPreview(std::string& a_profileName)
    {
        GetPresetPreviewCaches().erase(LowercaseKey(ProfileNameFromKey(a_profileName)));
    }

    static bool RebuildPresetPreview(
        std::string& a_profileName,
        PresetPreviewCache& a_preview,
        std::string& a_error)
    {
        a_preview.settings = "{}";
        a_preview.changedSettings = "{}";
        for (const auto& category : GetPresetCategories(a_profileName))
        {
            const auto selected = std::ranges::find_if(a_preview.presets, [&](const ActivePreset& a_preset)
                { return Config::IEquals(a_preset.category, category); });
            if (selected == a_preview.presets.end())
            {
                continue;
            }

            const auto path = TuningUtil::ProfileDirectory(a_profileName) / category / (selected->name + ".json");
            const auto text = ReadJsonText(path);
            const std::vector<std::string> metadataPath{ "autoLoadOnStartup" };
            const auto settings = text ? JsonOverlay::RemovePaths(*text, metadataPath, a_error) : std::nullopt;
            const auto merged = settings ? JsonOverlay::Overlay(a_preview.settings, *settings, a_error) : std::nullopt;
            if (!merged)
            {
                if (a_error.empty()) a_error = std::format("Preset {} could not be read.", path.string());
                return false;
            }
            a_preview.settings = std::move(*merged);

            if (a_preview.changedCategories.contains(LowercaseKey(category)))
            {
                const auto changed = JsonOverlay::Overlay(a_preview.changedSettings, *settings, a_error);
                if (!changed)
                {
                    return false;
                }
                a_preview.changedSettings = std::move(*changed);
            }
        }
        return true;
    }

    static bool WritePresetText(const std::filesystem::path& a_path, const std::string_view a_text)
    {
        std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
        file << a_text << '\n';
        return static_cast<bool>(file);
    }

    bool DisableAllAutoLoadPresets(std::size_t& a_updated, std::string& a_error)
    {
        struct PresetUpdate
        {
            std::filesystem::path path;
            std::string original;
            std::string updated;
        };

        a_updated = 0;
        a_error.clear();
        std::vector<PresetUpdate> updates;
        for (const auto& profile : TuningUtil::GetProfiles())
        {
            auto profileName = profile.name;
            for (const auto& category : GetPresetCategories(profileName))
            {
                for (const auto& preset : GetPresets(profileName, category))
                {
                    const auto path = profile.directory / category / (preset + ".json");
                    const auto original = ReadJsonText(path);
                    if (!original)
                    {
                        a_error = std::format("Preset {} could not be read.", path.string());
                        return false;
                    }
                    if (!JsonOverlay::BooleanMember(*original, "autoLoadOnStartup").value_or(false))
                    {
                        continue;
                    }
                    const auto updated = JsonOverlay::SetBooleanMember(
                        *original,
                        "autoLoadOnStartup",
                        false,
                        a_error);
                    if (!updated)
                    {
                        return false;
                    }
                    updates.push_back({ path, *original, *updated });
                }
            }
        }

        std::size_t written = 0;
        for (; written < updates.size(); ++written)
        {
            if (WritePresetText(updates[written].path, updates[written].updated))
            {
                continue;
            }
            for (std::size_t rollback = 0; rollback <= written; ++rollback)
            {
                (void)WritePresetText(updates[rollback].path, updates[rollback].original);
            }
            a_error = "A preset file could not be updated; the original files were restored where possible.";
            return false;
        }

        a_updated = updates.size();
        InvalidatePresetCache();
        logger::info("Disabled auto-load for {} TuningUtil preset(s)", a_updated);
        return true;
    }

    bool SavePreset(
        std::string& a_profileName,
        const std::string& a_categoryName,
        const std::string& a_presetName,
        std::string& a_error)
    {
        a_error.clear();
        if (ProfileNameFromKey(a_profileName).empty())
        {
            a_error = "The profile is unavailable.";
            return false;
        }
        const auto category = NormalizePresetPathComponent(a_categoryName, false, a_error);
        const auto presetName = NormalizePresetPathComponent(a_presetName, true, a_error);
        if (!category || !presetName)
        {
            return false;
        }

        const auto directory = TuningUtil::ProfileDirectory(a_profileName) / *category;
        const auto presetPath = directory / (*presetName + ".json");
        std::error_code existsError;
        const auto existingPreset = std::filesystem::is_regular_file(presetPath, existsError) ?
                                        ReadJsonText(presetPath) :
                                        std::nullopt;
        const auto autoLoadOnStartup = existingPreset ?
                                           JsonOverlay::BooleanMember(*existingPreset, "autoLoadOnStartup").value_or(false) :
                                           false;

        const auto settings = TuningUtil::SerializePresetSettings(a_profileName, a_error);
        const auto presetHeader = std::format(R"({{"autoLoadOnStartup":{}}})", autoLoadOnStartup);
        const auto text = settings ?
                              JsonOverlay::Overlay(presetHeader, *settings, a_error) :
                              std::nullopt;
        if (!text)
        {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            a_error = std::format("Could not create the preset folder: {}", error.message());
            return false;
        }

        auto temporaryPath = presetPath;
        temporaryPath += ".tmp";
        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            file << *text << '\n';
            if (!file)
            {
                file.close();
                std::error_code removeError;
                std::filesystem::remove(temporaryPath, removeError);
                a_error = "Could not write the preset file.";
                return false;
            }
        }
        if (!::MoveFileExW(
                temporaryPath.c_str(),
                presetPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const std::error_code moveError(static_cast<int>(::GetLastError()), std::system_category());
            std::error_code removeError;
            std::filesystem::remove(temporaryPath, removeError);
            a_error = std::format("Could not finish saving the preset: {}", moveError.message());
            return false;
        }

        const auto profileName = ProfileNameFromKey(a_profileName);
        InvalidatePresetCatalog(profileName, *category);
        GetActivePresetCaches().erase(LowercaseKey(profileName));
        logger::info("Saved TuningUtil preset {} in category {} for {}", *presetName, *category, a_profileName);
        return true;
    }

    bool PreviewPreset(
        std::string& a_profileName,
        const std::string& a_category,
        const std::string& a_presetName,
        std::string& a_error)
    {
        a_error.clear();
        const auto category = NormalizePresetPathComponent(a_category, false, a_error);
        const auto presetName = NormalizePresetPathComponent(a_presetName, true, a_error);
        if (ProfileNameFromKey(a_profileName).empty() || !category || !presetName)
        {
            if (a_error.empty()) a_error = "The profile is unavailable.";
            logger::warn("Rejected TuningUtil preset preview: {}", a_error);
            return false;
        }
        const auto presets = GetPresets(a_profileName, *category);
        const auto selected = std::ranges::find_if(presets, [&](const std::string& a_existing)
            { return Config::IEquals(a_existing, *presetName); });
        if (selected == presets.end())
        {
            a_error = "The preset file could not be found.";
            logger::warn("TuningUtil preset was not found: {} / {} / {}", a_profileName, *category, *presetName);
            return false;
        }

        const auto cacheKey = LowercaseKey(ProfileNameFromKey(a_profileName));
        PresetPreviewCache preview;
        if (const auto existing = GetPresetPreviewCaches().find(cacheKey); existing != GetPresetPreviewCaches().end())
        {
            preview = existing->second;
        }
        else
        {
            const auto* active = ResolveActivePresetCache(a_profileName, a_error);
            if (!active)
            {
                return false;
            }
            preview.presets = active->presets;
        }

        const auto previous = std::ranges::find_if(preview.presets, [&](const ActivePreset& a_preset)
            { return Config::IEquals(a_preset.category, *category); });
        const auto deselect = previous != preview.presets.end() &&
                              Config::IEquals(previous->name, *selected);
        if (deselect)
        {
            preview.presets.erase(previous);
        }
        else if (previous != preview.presets.end())
        {
            previous->category = *category;
            previous->name = *selected;
        }
        else
        {
            preview.presets.push_back({ *category, *selected });
        }
        preview.changedCategories.insert(LowercaseKey(*category));
        if (!RebuildPresetPreview(a_profileName, preview, a_error) ||
            !TuningUtil::ApplyPresetPreview(a_profileName, preview.settings, preview.changedSettings, a_error))
        {
            if (a_error.empty()) a_error = "The preset preview could not be applied.";
            return false;
        }

        GetPresetPreviewCaches().insert_or_assign(cacheKey, std::move(preview));
        if (deselect)
        {
            DetailedLogging::Info("Previewing no TuningUtil preset in category {} for {}", *category, a_profileName);
        }
        else
        {
            DetailedLogging::Info("Previewing TuningUtil preset {} in category {} for {}", *selected, *category, a_profileName);
        }
        return true;
    }

    bool CommitPresetPreviews(std::string& a_profileName, std::string& a_error)
    {
        a_error.clear();
        const auto cacheKey = LowercaseKey(ProfileNameFromKey(a_profileName));
        const auto preview = GetPresetPreviewCaches().find(cacheKey);
        if (preview == GetPresetPreviewCaches().end() || preview->second.changedCategories.empty())
        {
            DetailedLogging::Info("Preset selections for {} are already saved", a_profileName);
            return true;
        }

        struct PresetUpdate
        {
            std::filesystem::path path;
            std::string original;
            std::string updated;
        };
        std::vector<PresetUpdate> updates;
        const auto categories = GetPresetCategories(a_profileName);
        for (const auto& categoryKey : preview->second.changedCategories)
        {
            const auto category = std::ranges::find_if(categories, [&](const std::string& a_category)
                { return LowercaseKey(a_category) == categoryKey; });
            if (category == categories.end())
            {
                a_error = "A previewed preset category is unavailable.";
                return false;
            }
            const auto selected = std::ranges::find_if(preview->second.presets, [&](const ActivePreset& a_preset)
                { return LowercaseKey(a_preset.category) == categoryKey; });
            for (const auto& preset : GetPresets(a_profileName, *category))
            {
                const auto path = TuningUtil::ProfileDirectory(a_profileName) / *category / (preset + ".json");
                const auto original = ReadJsonText(path);
                const auto updated = original ? JsonOverlay::SetBooleanMember(
                                                    *original,
                                                    "autoLoadOnStartup",
                                                    selected != preview->second.presets.end() &&
                                                        Config::IEquals(preset, selected->name),
                                                    a_error) :
                                                std::nullopt;
                if (!original || !updated)
                {
                    if (a_error.empty()) a_error = "A preset file could not be updated.";
                    return false;
                }
                updates.push_back({ path, *original, *updated });
            }
        }

        std::size_t written = 0;
        for (; written < updates.size(); ++written)
        {
            if (WritePresetText(updates[written].path, updates[written].updated))
            {
                continue;
            }
            for (std::size_t rollback = 0; rollback <= written; ++rollback)
            {
                (void)WritePresetText(updates[rollback].path, updates[rollback].original);
            }
            a_error = "A preset file could not be updated; the original files were restored where possible.";
            return false;
        }

        GetActivePresetCaches().erase(cacheKey);
        if (!TuningUtil::ApplyPresetAndRemoveUserOverrides(
                a_profileName,
                preview->second.changedSettings,
                a_error))
        {
            for (const auto& update : updates)
            {
                (void)WritePresetText(update.path, update.original);
            }
            GetActivePresetCaches().erase(cacheKey);
            if (a_error.empty()) a_error = "Preset settings could not be saved; the original preset files were restored.";
            return false;
        }

        logger::info("Saved {} TuningUtil preset selection(s) for {}", preview->second.changedCategories.size(), a_profileName);
        GetPresetPreviewCaches().erase(preview);
        return true;
    }

    bool RemovePresets(
        std::string& a_profileName,
        const std::vector<std::string>& a_categories,
        const std::vector<ActivePreset>& a_presets,
        std::string& a_error)
    {
        a_error.clear();
        const auto profileName = ProfileNameFromKey(a_profileName);
        if (profileName.empty())
        {
            a_error = "The profile is unavailable.";
            return false;
        }

        struct Removal
        {
            std::filesystem::path original;
            std::filesystem::path staged;
        };
        std::vector<Removal> removals;
        const auto root = TuningUtil::ProfileDirectory(a_profileName);
        const auto suffix = std::format(".tuningutil-delete-{}", ::GetTickCount64());
        const auto addRemoval = [&](const std::filesystem::path& a_path)
        {
            if (std::ranges::any_of(removals, [&](const Removal& a_removal)
                    { return Config::IEquals(a_removal.original.string(), a_path.string()); }))
                return;
            auto staged = a_path;
            staged += suffix;
            removals.push_back({ a_path, std::move(staged) });
        };

        std::vector<std::string> normalizedCategories;
        for (const auto& categoryName : a_categories)
        {
            const auto category = NormalizePresetPathComponent(categoryName, false, a_error);
            if (!category) return false;
            normalizedCategories.push_back(*category);
            addRemoval(root / *category);
        }
        std::vector<ActivePreset> normalizedPresets;
        for (const auto& preset : a_presets)
        {
            const auto category = NormalizePresetPathComponent(preset.category, false, a_error);
            const auto name = NormalizePresetPathComponent(preset.name, true, a_error);
            if (!category || !name) return false;
            if (std::ranges::any_of(normalizedCategories, [&](const std::string& a_removedCategory)
                    { return Config::IEquals(a_removedCategory, *category); }))
                continue;
            normalizedPresets.push_back({ *category, *name });
            addRemoval(root / *category / (*name + ".json"));
        }

        std::size_t stagedCount = 0;
        for (; stagedCount < removals.size(); ++stagedCount)
        {
            std::error_code existsError;
            if (!std::filesystem::exists(removals[stagedCount].original, existsError) && !existsError)
            {
                continue;
            }
            std::error_code renameError;
            std::filesystem::rename(removals[stagedCount].original, removals[stagedCount].staged, renameError);
            if (!renameError)
            {
                continue;
            }
            for (std::size_t rollback = 0; rollback < stagedCount; ++rollback)
            {
                std::error_code rollbackError;
                if (std::filesystem::exists(removals[rollback].staged, rollbackError) && !rollbackError)
                    std::filesystem::rename(removals[rollback].staged, removals[rollback].original, rollbackError);
            }
            a_error = std::format("Could not remove a preset or category: {}", renameError.message());
            return false;
        }

        const auto cacheKey = LowercaseKey(profileName);
        const auto preview = GetPresetPreviewCaches().find(cacheKey);
        const auto previewAffected = preview != GetPresetPreviewCaches().end() &&
                                     (std::ranges::any_of(normalizedCategories, [&](const auto& a_category)
                                          { return preview->second.changedCategories.contains(LowercaseKey(a_category)); }) ||
                                         std::ranges::any_of(preview->second.presets, [&](const auto& a_active)
                                         {
                                             return std::ranges::any_of(normalizedCategories, [&](const auto& a_category)
                                                        { return Config::IEquals(a_category, a_active.category); }) ||
                                                    std::ranges::any_of(normalizedPresets, [&](const auto& a_preset)
                                                    {
                                                        return Config::IEquals(a_preset.category, a_active.category) &&
                                                               Config::IEquals(a_preset.name, a_active.name);
                                                    });
                                         }));
        std::erase_if(GetPresetCatalogs(), [&](const auto& a_entry)
            { return a_entry.first.starts_with(cacheKey + "|"); });
        GetActivePresetCaches().erase(cacheKey);
        if (previewAffected) GetPresetPreviewCaches().erase(cacheKey);
        for (const auto& removal : removals)
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(removal.staged, cleanupError);
            if (cleanupError)
            {
                logger::warn("Could not delete staged preset data {}: {}", removal.staged.string(), cleanupError.message());
            }
        }
        logger::info(
            "Removed {} TuningUtil preset categor{} and {} individual preset(s) for {}",
            a_categories.size(),
            a_categories.size() == 1 ? "y" : "ies",
            a_presets.size(),
            profileName);
        return true;
    }

}  // namespace MPL::WeatherPatcher
