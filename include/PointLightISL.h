#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace MPL::PointLightISL
{
    inline constexpr std::uint32_t recordFlag = 1u << 14;
    inline constexpr std::uint32_t initializedFlag = 1u << 8;
    inline constexpr std::uint32_t runtimeFlag = 1u << 10;

    template <class Runtime>
    bool HasExtension(const Runtime& a_runtime)
    {
        // CS stores flags, cutoff and the base FormID in NiLight's ambient field.
        std::uint32_t flags;
        std::memcpy(&flags, &a_runtime.ambient, sizeof(flags));
        return (flags & initializedFlag) != 0;
    }

    template <class Runtime>
    bool IsInverseSquare(const Runtime& a_runtime)
    {
        std::uint32_t flags;
        std::memcpy(&flags, &a_runtime.ambient, sizeof(flags));
        return (flags & (initializedFlag | runtimeFlag)) == (initializedFlag | runtimeFlag);
    }

    template <class Runtime>
    float& SourceSize(Runtime& a_runtime)
    {
        // CS replaces the vanilla radius vector with radius, padding and source size.
        return a_runtime.radius.z;
    }

    struct FadeBaseline
    {
        float sourceOffset = 0.0f;
        float appliedOffset = 0.0f;
        bool hasApplied = false;

        void Capture(const float a_currentOffset)
        {
            if (!hasApplied || a_currentOffset != appliedOffset)
                sourceOffset = a_currentOffset;
        }

        float Intensity(const float a_baseFade) const
        {
            return std::max(0.0f, a_baseFade + sourceOffset);
        }
    };

    struct Adjustment
    {
        float intensity;
        float size;
    };

    inline std::optional<Adjustment> Scale(
        const float a_intensity, const float a_size,
        const double a_brightness, const double a_radius)
    {
        if (!std::isfinite(a_intensity) || !std::isfinite(a_size) || a_size <= 0.0f ||
            !std::isfinite(a_brightness) || !std::isfinite(a_radius)) return std::nullopt;
        const auto radius = std::max(0.0, a_radius);
        // Scaling intensity by R squared and source size by R preserves peak brightness.
        const auto intensity = std::max(0.0, static_cast<double>(a_intensity)) *
                               std::max(0.0, a_brightness) * radius * radius;
        const auto size = radius > 0.0 ? static_cast<double>(a_size) * radius : a_size;
        constexpr auto maximum = static_cast<double>(std::numeric_limits<float>::max());
        if (!std::isfinite(intensity) || intensity > maximum / 4.0 ||
            !std::isfinite(size) || size <= 0.0 || size * size > maximum) return std::nullopt;
        return Adjustment{ static_cast<float>(intensity), static_cast<float>(size) };
    }

    inline float RuntimeFade(
        const float a_runtimeFade, const float a_previousIntensity,
        const float a_intensity, float& a_flicker)
    {
        if (a_previousIntensity == a_intensity) return a_runtimeFade;
        if (a_previousIntensity > 0.0f && std::isfinite(a_runtimeFade))
        {
            const auto flicker = a_runtimeFade / a_previousIntensity;
            if (std::isfinite(flicker)) a_flicker = std::max(0.0f, flicker);
        }
        const auto fade = static_cast<double>(a_intensity) * a_flicker;
        return static_cast<float>(std::clamp(fade, 0.0,
            static_cast<double>(std::numeric_limits<float>::max()) / 4.0));
    }
}
