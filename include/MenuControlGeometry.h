#pragma once

#include <MenuBoxGeometry.h>

#include <array>
#include <cstdint>

namespace MPL::MenuControlGeometry
{
    using Point = MenuBoxGeometry::Point;
    using Rect = MenuBoxGeometry::BorderRect;

    struct Vertex
    {
        Point position;
        Point uv;
        std::uint32_t color;
    };

    struct Polygon
    {
        std::array<Vertex, 8> vertices{};
        std::size_t count = 0;
    };

    constexpr float CornerDrop(const Rect a_rect, const float a_frameHeight)
    {
        return std::max(0.0f, std::min({
            MenuBoxGeometry::CollapsedHeaderDrop(a_frameHeight, a_rect.maximum.x - a_rect.minimum.x),
            (a_rect.maximum.x - a_rect.minimum.x) / (2.0f * MenuBoxGeometry::SlopeRunPerRise),
            (a_rect.maximum.y - a_rect.minimum.y) * 0.5f }));
    }

    struct Corners
    {
        bool left;
        bool right;
    };

    inline constexpr Corners SliderCorners{ true, false };
    inline constexpr Corners SliderValueCorners{ false, true };

    inline Vertex Interpolate(const Vertex& a_first, const Vertex& a_second, const float a_amount)
    {
        const auto mix = [&](const float a_left, const float a_right)
        { return a_left + (a_right - a_left) * a_amount; };
        std::uint32_t color = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            const auto channel = std::lround(mix(static_cast<float>((a_first.color >> shift) & 255),
                static_cast<float>((a_second.color >> shift) & 255)));
            color |= static_cast<std::uint32_t>(std::clamp(channel, 0L, 255L)) << shift;
        }
        return {
            { mix(a_first.position.x, a_second.position.x), mix(a_first.position.y, a_second.position.y) },
            { mix(a_first.uv.x, a_second.uv.x), mix(a_first.uv.y, a_second.uv.y) },
            color,
        };
    }

    inline Polygon ClipCorner(const Polygon& a_polygon, const Rect a_rect, const float a_drop, const bool a_left)
    {
        if (a_polygon.count == 0 || a_drop <= 0.0f) return a_polygon;
        const auto run = a_drop * MenuBoxGeometry::SlopeRunPerRise;
        auto minX = a_polygon.vertices[0].position.x;
        auto maxX = minX;
        auto minY = a_polygon.vertices[0].position.y;
        auto maxY = minY;
        for (std::size_t index = 1; index < a_polygon.count; ++index)
        {
            const auto point = a_polygon.vertices[index].position;
            minX = std::min(minX, point.x);
            maxX = std::max(maxX, point.x);
            minY = std::min(minY, point.y);
            maxY = std::max(maxY, point.y);
        }
        // Labels outside the frame are not part of its corner.
        if (minX > a_rect.maximum.x || maxX < a_rect.minimum.x ||
            minY > a_rect.maximum.y || maxY < a_rect.minimum.y ||
            (a_left ? minX >= a_rect.minimum.x + run || maxY <= a_rect.maximum.y - a_drop :
                      maxX <= a_rect.maximum.x - run || minY >= a_rect.minimum.y + a_drop))
            return a_polygon;

        const auto distance = [&](const Point a_point)
        {
            return a_left ?
                       a_point.x - a_rect.minimum.x +
                           (a_rect.maximum.y - a_drop - a_point.y) * MenuBoxGeometry::SlopeRunPerRise :
                       a_rect.maximum.x - run - a_point.x +
                           (a_point.y - a_rect.minimum.y) * MenuBoxGeometry::SlopeRunPerRise;
        };
        Polygon result;
        auto previous = a_polygon.vertices[a_polygon.count - 1];
        auto previousDistance = distance(previous.position);
        for (std::size_t index = 0; index < a_polygon.count; ++index)
        {
            const auto current = a_polygon.vertices[index];
            const auto currentDistance = distance(current.position);
            if ((previousDistance >= 0.0f) != (currentDistance >= 0.0f))
                result.vertices[result.count++] = Interpolate(previous, current,
                    previousDistance / (previousDistance - currentDistance));
            if (currentDistance >= 0.0f) result.vertices[result.count++] = current;
            previous = current;
            previousDistance = currentDistance;
        }
        return result;
    }
}
