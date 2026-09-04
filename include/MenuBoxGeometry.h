#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace MPL::MenuBoxGeometry
{
    inline constexpr float BorderWidth = 1.0f;
    inline constexpr float AccentWidth = 10.0f;
    inline constexpr float MiddleBarHeightRatio = 0.3f;
    inline constexpr float CollapsedCornerScale = 1.2f;
    inline constexpr float SlopeRunPerRise = 1.7320508f;
    inline constexpr float SlopeLengthPerRise = 2.0f;
    inline constexpr float VerticalMiter = (SlopeLengthPerRise - 1.0f) / SlopeRunPerRise;

    constexpr float CollapsedHeaderDrop(const float a_height, const float a_width)
    {
        const auto availableRun = a_width - BorderWidth * 2.0f;
        return std::max(0.0f, std::min(a_height * MiddleBarHeightRatio * CollapsedCornerScale, availableRun / SlopeRunPerRise));
    }

    constexpr float CornerDrop(const float a_headerHeight, const float a_width)
    {
        const auto availableRun = a_width - BorderWidth -
                                  (BorderWidth + AccentWidth) * SlopeLengthPerRise +
                                  BorderWidth * SlopeRunPerRise;
        return std::max(0.0f, std::min(CollapsedHeaderDrop(a_headerHeight, a_width), availableRun / SlopeRunPerRise));
    }

    constexpr float CornerClearance(const float a_drop, const float a_accentWidth = AccentWidth)
    {
        return a_drop > 0.0f ? a_drop + (BorderWidth + a_accentWidth) * VerticalMiter : 0.0f;
    }

    constexpr float ContentSideGap(const float a_leftInset)
    {
        return std::max(0.0f, a_leftInset - BorderWidth - AccentWidth);
    }

    constexpr float PlainBoxContentInset(const float a_leftInset)
    {
        return BorderWidth + ContentSideGap(a_leftInset);
    }

    constexpr float HeaderContentTop(const float a_headerBottom, const float a_leftInset)
    {
        return a_headerBottom + ContentSideGap(a_leftInset);
    }

    constexpr float SpaceElementHeight(const float a_horizontalInset, const float a_itemSpacing, const bool a_small)
    {
        const auto spacing = a_horizontalInset * (a_small ? 0.5f : 1.0f);
        return std::max(0.0f, spacing - a_itemSpacing);
    }

    inline float NestedTopInset(
        const float a_cursorY,
        const std::optional<float> a_headerContentTop,
        const float a_defaultInset)
    {
        constexpr float positionTolerance = 0.01f;
        return a_headerContentTop && std::abs(a_cursorY - *a_headerContentTop) < positionTolerance ?
                   0.0f : a_defaultInset;
    }

    constexpr float InsetBevelDrop(const float a_drop, const float a_inset)
    {
        return std::max(0.0f, a_drop - a_inset * (1.0f - VerticalMiter));
    }

    constexpr float CollapsedHeaderFillHeight(const float a_top, const float a_height, const float a_boxBottom)
    {
        return std::max(0.0f, std::min(a_height, a_boxBottom - BorderWidth - a_top));
    }

    constexpr float ExposedTopBorderWidth(const float a_left, const float a_right, const float a_previousBorderStart)
    {
        return std::clamp(a_previousBorderStart - a_left, 0.0f, std::max(0.0f, a_right - a_left));
    }

    struct Point
    {
        float x;
        float y;
    };

    constexpr Point DropdownArrowPoint(const float a_side, const float a_y, const float a_width, const bool a_open)
    {
        const auto x = a_side * (0.5f - a_y) * 0.5f * a_width;
        const auto y = a_y * a_width * SlopeRunPerRise * 0.5f;
        return a_open ? Point{ x, y } : Point{ -y, x };
    }

    constexpr float DropdownArrowCenterX(const float a_cornerEndX, const float a_arrowWidth)
    {
        return a_cornerEndX - DropdownArrowPoint(1.0f, -0.5f, a_arrowWidth, false).x;
    }

    struct BorderRect
    {
        Point minimum;
        Point maximum;
    };

    constexpr BorderRect ExposedTopBorder(const float a_left, const float a_right, const Point a_previousBorderStart)
    {
        const auto width = ExposedTopBorderWidth(a_left, a_right, a_previousBorderStart.x);
        return {
            { a_left, a_previousBorderStart.y - BorderWidth * 0.5f },
            { a_left + width, a_previousBorderStart.y + BorderWidth * 0.5f },
        };
    }

    struct Bevel
    {
        float left;
        float bottom;
        float drop;

        constexpr Point Turn(const float a_inset) const
        {
            return { left + a_inset, bottom - drop - a_inset * VerticalMiter };
        }

        constexpr Point End(const float a_inset, const float a_bottomInset) const
        {
            return {
                left + drop * SlopeRunPerRise + a_inset * SlopeLengthPerRise -
                    a_bottomInset * SlopeRunPerRise,
                bottom - a_bottomInset,
            };
        }
    };

    constexpr Bevel MakeBevel(
        const float a_left,
        const float a_bottom,
        const float a_headerBottom,
        const float a_drop,
        const float a_accentWidth = AccentWidth)
    {
        const auto availableDrop = a_bottom - a_headerBottom -
                                   (BorderWidth + a_accentWidth) * VerticalMiter;
        return { a_left, a_bottom, std::max(0.0f, std::min(a_drop, availableDrop)) };
    }

    struct ClosedBoxFooter
    {
        float bottom;
        float cursorY;
        float leftInset;
        float cornerDrop = 0.0f;
    };

    inline std::optional<float> NestedBottomInset(
        const std::optional<ClosedBoxFooter>& a_child,
        const float a_lastItemBottom,
        const float a_cursorY,
        const float a_leftAccentWidth)
    {
        constexpr float positionTolerance = 0.01f;
        if (!a_child || std::abs(a_child->bottom - a_lastItemBottom) > positionTolerance ||
            std::abs(a_child->cursorY - a_cursorY) > positionTolerance)
        {
            return std::nullopt;
        }
        return std::max(0.0f, a_child->leftInset - a_leftAccentWidth);
    }

    constexpr float CornerDropAboveNestedBox(
        const float a_drop,
        const float a_leftInset,
        const float a_bottomInset,
        const float a_childCornerDrop,
        const float a_accentWidth = AccentWidth)
    {
        const auto innerAccent = BorderWidth + a_accentWidth;
        const auto availableDrop = a_bottomInset + a_childCornerDrop + (a_leftInset - innerAccent) / SlopeRunPerRise -
                                   innerAccent * VerticalMiter - BorderWidth;
        return std::max(0.0f, std::min(a_drop, availableDrop));
    }
}
