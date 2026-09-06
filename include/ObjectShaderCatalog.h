#pragma once

#include <cstdint>
#include <string_view>

namespace MPL::ObjectShaderCatalog
{
    enum class Capability : std::uint8_t
    {
        none = 0,
        lighting = 1U << 0,
        effect = 1U << 1,
    };

    constexpr Capability operator|(const Capability a_left, const Capability a_right)
    {
        return static_cast<Capability>(static_cast<std::uint8_t>(a_left) | static_cast<std::uint8_t>(a_right));
    }

    constexpr bool Has(const Capability a_available, const Capability a_required)
    {
        const auto available = static_cast<std::uint8_t>(a_available);
        const auto required = static_cast<std::uint8_t>(a_required);
        return required != 0 && (available & required) == required;
    }

    Capability Get(std::string_view a_modelPath);
}
