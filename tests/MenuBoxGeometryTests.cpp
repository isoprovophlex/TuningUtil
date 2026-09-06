#include <MenuBoxGeometry.h>
#include <MenuControlGeometry.h>

#include <array>
#include <iostream>
#include <numbers>

namespace
{
    bool Expect(const bool a_condition, const char* a_message)
    {
        if (!a_condition) std::cerr << "FAILED: " << a_message << '\n';
        return a_condition;
    }

    bool Near(const float a_left, const float a_right)
    {
        return std::abs(a_left - a_right) < 0.001f;
    }
}

int main()
{
    namespace Geometry = MPL::MenuBoxGeometry;
    namespace Controls = MPL::MenuControlGeometry;
    bool passed = true;
    const Controls::Rect value{ { 454.0f, 100.0f }, { 534.0f, 138.0f } };
    passed &= Expect(Controls::SliderCorners.left && !Controls::SliderCorners.right,
        "all sliders and compression gauges retain only the left corner cut");
    passed &= Expect(!Controls::SliderValueCorners.left && Controls::SliderValueCorners.right,
        "only an explicit slider value box receives the trailing corner");
    const auto area = [](const Controls::Polygon& a_polygon)
    {
        auto result = 0.0f;
        for (std::size_t index = 0; index < a_polygon.count; ++index)
        {
            const auto first = a_polygon.vertices[index].position;
            const auto second = a_polygon.vertices[(index + 1) % a_polygon.count].position;
            result += first.x * second.y - first.y * second.x;
        }
        return std::abs(result) * 0.5f;
    };
    for (const auto width : std::array{ 1.0f, 12.0f, 38.0f, 80.0f, 420.0f })
    {
        for (const auto height : std::array{ 1.0f, 20.0f, 38.0f, 150.0f, 630.0f })
        {
            const Controls::Rect rect{ { 0.0f, 0.0f }, { width, height } };
            const auto drop = Controls::CornerDrop(rect, 38.0f);
            const auto run = drop * Geometry::SlopeRunPerRise;
            passed &= Expect(drop >= 0.0f && run <= width * 0.5f && drop <= height * 0.5f,
                "narrow controls cannot have overlapping or inverted corners");
            passed &= Expect(drop == 0.0f || Near(std::atan2(drop, run) * 180.0f / std::numbers::pi_v<float>, 30.0f),
                "control corners retain the shared thirty-degree angle");
            const Controls::Vertex topLeft{ { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0x804080C0 };
            const Controls::Vertex topRight{ { width, 0.0f }, { 1.0f, 0.0f }, 0x804080C0 };
            const Controls::Vertex bottomRight{ { width, height }, { 1.0f, 1.0f }, 0x804080C0 };
            const Controls::Vertex bottomLeft{ { 0.0f, height }, { 0.0f, 1.0f }, 0x804080C0 };
            for (const auto left : std::array{ false, true })
            {
                for (const auto right : std::array{ false, true })
                {
                    auto clippedArea = 0.0f;
                    for (auto polygon : std::array{
                             Controls::Polygon{ { topLeft, topRight, bottomRight }, 3 },
                             Controls::Polygon{ { topLeft, bottomRight, bottomLeft }, 3 } })
                    {
                        if (left) polygon = Controls::ClipCorner(polygon, rect, drop, true);
                        if (right) polygon = Controls::ClipCorner(polygon, rect, drop, false);
                        clippedArea += area(polygon);
                        for (std::size_t index = 0; index < polygon.count; ++index)
                        {
                            const auto& vertex = polygon.vertices[index];
                            passed &= Expect(vertex.color == 0x804080C0, "corner clipping preserves color and transparency");
                            passed &= Expect(Near(vertex.uv.x, vertex.position.x / width) &&
                                                 Near(vertex.uv.y, vertex.position.y / height),
                                "clipped geometry preserves texture interpolation");
                        }
                    }
                    const auto expectedArea = width * height - (static_cast<int>(left) + static_cast<int>(right)) * drop * run * 0.5f;
                    passed &= Expect(std::abs(clippedArea - expectedArea) < std::max(0.001f, expectedArea * 0.00001f),
                        "only the requested outer corners are cut from each control");
                }
            }
        }
    }
    const Controls::Polygon label{ { Controls::Vertex{ { 540, 102 }, {}, 0xFFFFFFFF },
                                      Controls::Vertex{ { 600, 102 }, {}, 0xFFFFFFFF },
                                      Controls::Vertex{ { 540, 124 }, {}, 0xFFFFFFFF } }, 3 };
    passed &= Expect(Near(area(Controls::ClipCorner(label, value, 13.0f, false)), area(label)),
        "labels to the right do not get cut or redefine a row endpoint");
    for (const auto horizontalInset : std::array{ 16.0f, 26.0f, 36.0f })
    {
        for (const auto spacing : std::array{ 0.0f, 4.0f, 8.0f })
        {
            const auto full = Geometry::SpaceElementHeight(horizontalInset, spacing, false);
            const auto small = Geometry::SpaceElementHeight(horizontalInset, spacing, true);
            passed &= Expect(Near(full + spacing, horizontalInset),
                "Space matches the shared horizontal inset including automatic item spacing");
            passed &= Expect(Near(small + spacing, (full + spacing) * 0.5f),
                "Space Small occupies half the normal space including automatic item spacing");
        }
    }
    passed &= Expect(Near(Geometry::SpaceElementHeight(26.0f, 4.0f, false) + 4.0f, 26.0f) &&
                         Near(Geometry::SpaceElementHeight(26.0f, 4.0f, true) + 4.0f, 13.0f),
        "the current slider inset produces twenty-six-pixel and thirteen-pixel space elements");
    for (const auto inset : std::array{ 16.0f, 26.0f, 36.0f })
    {
        for (const auto headerBottom : std::array{ 49.0f, 93.0f, 250.0f })
        {
            const auto contentTop = Geometry::HeaderContentTop(headerBottom, inset);
            const auto plainInset = Geometry::PlainBoxContentInset(inset);
            passed &= Expect(Near(plainInset - Geometry::BorderWidth, contentTop - headerBottom),
                "plain box top and side padding match the clear drop box header gap");
            passed &= Expect(Near(contentTop - headerBottom, inset - Geometry::BorderWidth - Geometry::AccentWidth),
                "the first control's top gap matches the clear gap beside the rail");
            passed &= Expect(Geometry::NestedTopInset(contentTop, contentTop, 3.0f) == 0.0f,
                "a first nested box does not add padding on top of the matched header gap");
            passed &= Expect(Geometry::NestedTopInset(contentTop + 40.0f, contentTop, 3.0f) == 3.0f,
                "nested boxes after other content retain their existing spacing");
        }
    }
    passed &= Expect(Geometry::HeaderContentTop(49.0f, 26.0f) == 64.0f,
        "the current twenty-six-pixel content inset gives a fifteen-pixel header gap");
    passed &= Expect(Geometry::HeaderContentTop(49.0f, 8.0f) == 49.0f,
        "small content insets cannot move controls into the header");
    passed &= Expect(Geometry::NestedTopInset(64.0f, std::nullopt, 3.0f) == 3.0f,
        "unheaded boxes retain their existing top spacing");
    for (const auto width : std::array{ 6.0f, 8.0f, 13.64f, 20.585f, 33.12f, 64.0f })
    {
        const auto left = Geometry::DropdownArrowPoint(-1.0f, -0.5f, width, true);
        const auto right = Geometry::DropdownArrowPoint(1.0f, -0.5f, width, true);
        const auto tip = Geometry::DropdownArrowPoint(0.0f, 0.5f, width, true);
        passed &= Expect(Near(right.x - left.x, width) && Near(std::hypot(tip.x - left.x, tip.y - left.y), width) &&
                             Near(std::hypot(tip.x - right.x, tip.y - right.y), width),
            "the dropdown arrow has three equal sides at every size");
        for (const auto band : std::array{ std::array{ -0.5f, -0.3f }, std::array{ -0.08f, 0.12f }, std::array{ 0.27f, 0.5f } })
        {
            for (const auto side : std::array{ -1.0f, 1.0f })
            {
                for (const auto open : std::array{ false, true })
                {
                    const auto start = Geometry::DropdownArrowPoint(side, band[0], width, open);
                    const auto end = Geometry::DropdownArrowPoint(side, band[1], width, open);
                    const auto angle = std::atan2(std::abs(end.y - start.y), std::abs(end.x - start.x)) *
                                       180.0f / std::numbers::pi_v<float>;
                    passed &= Expect(Near(angle, open ? 60.0f : 30.0f),
                        "every colored band follows the same equilateral edge and matches the corner when collapsed");
                }
                const auto openPoint = Geometry::DropdownArrowPoint(side, band[0], width, true);
                const auto closedPoint = Geometry::DropdownArrowPoint(side, band[0], width, false);
                passed &= Expect(Near(closedPoint.x, -openPoint.y) && Near(closedPoint.y, openPoint.x),
                    "opening the arrow only rotates its geometry around the same center");
            }
        }
    }
    for (const auto height : std::array{ 30.0f, 44.75f, 72.0f })
    {
        const auto drop = Geometry::CornerDrop(height, 600.0f);
        const auto bevel = Geometry::MakeBevel(20.0f, 500.0f, 80.0f, drop);
        const auto turn = bevel.Turn(0.0f);
        const auto end = bevel.End(0.0f, 0.0f);
        const auto angle = std::atan2(end.y - turn.y, end.x - turn.x) * 180.0f / std::numbers::pi_v<float>;
        passed &= Expect(Near(drop, Geometry::CollapsedHeaderDrop(height, 600.0f)),
            "expanded and collapsed corners use the same cutout size at every font scale");
        for (const auto boxWidth : std::array{ 120.0f, 600.0f, 1200.0f })
        {
            passed &= Expect(Near(Geometry::CornerDrop(height, boxWidth), Geometry::CollapsedHeaderDrop(height, boxWidth)),
                "expanded corner size matches the collapsed cut across normal menu widths");
        }
        passed &= Expect(Near(angle, 30.0f), "corner uses the header's 30-degree slope");

        const auto outer = bevel.Turn(Geometry::BorderWidth);
        const auto inner = bevel.Turn(Geometry::BorderWidth + Geometry::AccentWidth);
        const auto perpendicularWidth = ((inner.x - outer.x) -
                                            (inner.y - outer.y) * Geometry::SlopeRunPerRise) /
                                        Geometry::SlopeLengthPerRise;
        passed &= Expect(Near(perpendicularWidth, Geometry::AccentWidth), "diagonal maintains the ten-pixel accent thickness");
        passed &= Expect(inner.y >= bevel.bottom - Geometry::CornerClearance(drop) - 0.001f,
            "reserved footer keeps the inner miter below the content");

        const auto collapsedDrop = Geometry::CollapsedHeaderDrop(height, 600.0f);
        const auto plainBox = Geometry::MakeBevel(20.0f, 500.0f, 80.0f, collapsedDrop, 0.0f);
        passed &= Expect(Near(plainBox.drop, collapsedDrop), "standard boxes use the collapsed cutout size");
        const auto plainCornerTurn = plainBox.Turn(Geometry::BorderWidth);
        passed &= Expect(Near(plainCornerTurn.y, plainBox.bottom - Geometry::CornerClearance(collapsedDrop, 0.0f)),
            "standard box footer clearance follows its thin outline without reserving a colored rail");
        const auto plainInset = Geometry::PlainBoxContentInset(26.0f);
        const auto plainNestedDrop = Geometry::CornerDropAboveNestedBox(collapsedDrop, plainInset, plainInset, 0.0f, 0.0f);
        const Geometry::Bevel plainNestedBox{ 0.0f, 500.0f, plainNestedDrop };
        const auto plainInnerTurn = plainNestedBox.Turn(Geometry::BorderWidth);
        const auto plainBorderAtChildLeft = plainInnerTurn.y + (plainInset - plainInnerTurn.x) / Geometry::SlopeRunPerRise;
        passed &= Expect(plainNestedDrop <= collapsedDrop &&
                             plainBorderAtChildLeft >= plainNestedBox.bottom - plainInset + Geometry::BorderWidth - 0.001f,
            "standard nested box corners stay below their child without enlarging the cutout");
        const Geometry::Bevel collapsed{ 20.0f, 500.0f, collapsedDrop };
        const auto collapsedTurn = collapsed.Turn(0.0f);
        const auto collapsedEnd = collapsed.End(0.0f, 0.0f);
        const auto arrowWidth = std::max(8.0f, height * 0.46f);
        const auto cornerEnd = collapsed.End(Geometry::BorderWidth * 0.5f, Geometry::BorderWidth * 0.5f);
        const auto arrowCenterX = Geometry::DropdownArrowCenterX(cornerEnd.x, arrowWidth);
        for (const auto side : std::array{ -1.0f, 1.0f })
        {
            const auto arrowCorner = Geometry::DropdownArrowPoint(side, -0.5f, arrowWidth, false);
            passed &= Expect(Near(arrowCenterX + arrowCorner.x, cornerEnd.x),
                "both right arrow corners align with the cutout endpoint at every header scale");
        }
        const auto nestedArrowCenterX = Geometry::DropdownArrowCenterX(cornerEnd.x + 26.0f, arrowWidth);
        passed &= Expect(Near(nestedArrowCenterX - arrowCenterX, 26.0f),
            "arrow alignment follows the box when it is nested");
        const auto collapsedAngle = std::atan2(collapsedEnd.y - collapsedTurn.y, collapsedEnd.x - collapsedTurn.x) *
                                    180.0f / std::numbers::pi_v<float>;
        passed &= Expect(Near(collapsedDrop, height * Geometry::MiddleBarHeightRatio * 1.2f),
            "collapsed cut is twenty percent larger at every font scale");
        passed &= Expect(Near(collapsedAngle, 30.0f), "collapsed cut retains the 30-degree slope at every font scale");
        passed &= Expect(collapsedTurn.y >= collapsed.bottom - height,
            "collapsed cut stays inside the header without a footer");

        const Geometry::Bevel headerFill{
            collapsed.left + Geometry::BorderWidth,
            collapsed.bottom - Geometry::BorderWidth,
            Geometry::InsetBevelDrop(collapsedDrop, Geometry::BorderWidth),
        };
        const auto fillTurn = headerFill.Turn(0.0f);
        const auto fillEnd = headerFill.End(0.0f, 0.0f);
        const auto borderTurn = collapsed.Turn(Geometry::BorderWidth);
        const auto borderEnd = collapsed.End(Geometry::BorderWidth, Geometry::BorderWidth);
        passed &= Expect(Near(fillTurn.x, borderTurn.x) && Near(fillTurn.y, borderTurn.y) &&
                             Near(fillEnd.x, borderEnd.x) && Near(fillEnd.y, borderEnd.y),
            "collapsed fill follows the inside edge of the thin perimeter");

        const auto headerTop = 4.25f;
        const auto boxBottom = std::floor(headerTop + height);
        const auto fillHeight = Geometry::CollapsedHeaderFillHeight(headerTop, height, boxBottom);
        const Geometry::Bevel pixelBorder{ 11.0f, boxBottom, collapsedDrop };
        const Geometry::Bevel pixelFill{
            pixelBorder.left + Geometry::BorderWidth,
            headerTop + fillHeight,
            Geometry::InsetBevelDrop(collapsedDrop, Geometry::BorderWidth),
        };
        passed &= Expect(Near(pixelFill.Turn(0.0f).y, pixelBorder.Turn(Geometry::BorderWidth).y) &&
                             Near(pixelFill.End(0.0f, 0.0f).x, pixelBorder.End(Geometry::BorderWidth, Geometry::BorderWidth).x),
            "fractional header heights cannot paint over the pixel-aligned diagonal border");
        const auto nextTopWidth = Geometry::ExposedTopBorderWidth(
            pixelBorder.left, pixelBorder.left + 600.0f, pixelBorder.End(0.0f, 0.0f).x);
        passed &= Expect(Near(nextTopWidth, collapsedDrop * Geometry::SlopeRunPerRise),
            "the next header restores only the top edge exposed by the preceding cut");
    }
    for (const auto width : std::array{ 1.0f, 2.0f, 8.0f, 22.0f, 40.0f, 600.0f })
    {
        const auto drop = Geometry::CollapsedHeaderDrop(44.75f, width);
        passed &= Expect(drop >= 0.0f && drop <= 44.75f * Geometry::MiddleBarHeightRatio * Geometry::CollapsedCornerScale,
            "narrow collapsed headers never enlarge or invert the cut");
        passed &= Expect(drop * Geometry::SlopeRunPerRise <= std::max(0.0f, width - Geometry::BorderWidth * 2.0f) + 0.001f,
            "collapsed cut leaves room for the right border in narrow boxes");
    }
    passed &= Expect(Geometry::InsetBevelDrop(0.0f, Geometry::BorderWidth) == 0.0f,
        "expanded header backgrounds retain their square bottom-left corner");
    passed &= Expect(Geometry::CollapsedHeaderFillHeight(4.0f, 44.75f, 200.0f) == 44.75f,
        "a collapsing window does not stretch the header to its previous expanded height");
    passed &= Expect(Geometry::CollapsedHeaderFillHeight(4.0f, 44.75f, 4.0f) == 0.0f,
        "clipped headers cannot produce a negative fill height");
    passed &= Expect(Geometry::ExposedTopBorderWidth(11.0f, 600.0f, 11.0f) == 0.0f,
        "square stacked boxes keep a single shared divider");
    passed &= Expect(Geometry::ExposedTopBorderWidth(40.0f, 600.0f, 34.0f) == 0.0f,
        "an inset next box does not redraw an already covered top edge");
    passed &= Expect(Geometry::ExposedTopBorderWidth(11.0f, 20.0f, 34.0f) == 9.0f,
        "restored top edges stay inside narrow boxes");

    for (const auto drop : std::array{ 0.0f, Geometry::CollapsedHeaderDrop(44.75f, 600.0f), Geometry::CornerDrop(44.75f, 600.0f) })
    {
        for (const auto bottom : std::array{ 16.0f, 48.0f, 93.25f })
        {
            const Geometry::Bevel previousBox{ 6.0f, bottom, drop };
            const auto halfBorder = Geometry::BorderWidth * 0.5f;
            const auto previousStart = drop > 0.0f ? previousBox.End(halfBorder, halfBorder) :
                                                    Geometry::Point{ previousBox.left, bottom - halfBorder };
            const auto exposed = Geometry::ExposedTopBorder(previousBox.left, 600.0f, previousStart);
            passed &= Expect(Near(exposed.minimum.y, bottom - Geometry::BorderWidth) &&
                                 Near(exposed.maximum.y, bottom),
                "shared border segments occupy the same pixel row, including fractional positions");
            passed &= Expect(Near(exposed.maximum.x, previousStart.x),
                "the exposed line meets the actual diagonal stroke endpoint");
            passed &= Expect(Near((exposed.minimum.y + exposed.maximum.y) * 0.5f, previousStart.y),
                "the shared line uses the preceding border center rather than the next box top");
            const auto joinedBoxTop = previousBox.bottom - Geometry::BorderWidth;
            const auto headerBackgroundTop = joinedBoxTop + Geometry::BorderWidth;
            passed &= Expect(Near(headerBackgroundTop, exposed.maximum.y),
                "joined headers begin immediately after the shared border without an empty pixel row");
            passed &= Expect(Near(std::max(joinedBoxTop, previousStart.y + halfBorder), headerBackgroundTop),
                "joined child backgrounds preserve the existing shared border row");
            if (drop == 0.0f)
                passed &= Expect(exposed.maximum.x == exposed.minimum.x,
                    "square box dividers are not painted twice");
        }
    }

    for (const auto width : std::array{ 8.0f, 22.0f, 40.0f, 100.0f, 600.0f })
    {
        const auto drop = Geometry::CornerDrop(44.75f, width);
        passed &= Expect(drop >= 0.0f && drop <= Geometry::CollapsedHeaderDrop(44.75f, width),
            "narrow boxes never enlarge or invert the corner");
        if (drop > 0.0f)
        {
            const Geometry::Bevel bevel{ 0.0f, 500.0f, drop };
            passed &= Expect(bevel.End(Geometry::BorderWidth + Geometry::AccentWidth, Geometry::BorderWidth).x <=
                                 width - Geometry::BorderWidth + 0.001f,
                "corner tip stays inside a narrow box");
        }
    }
    const auto shortBox = Geometry::MakeBevel(0.0f, 101.0f, 100.0f, 30.0f);
    passed &= Expect(shortBox.drop == 0.0f, "an opening box cannot draw a corner through its header");
    passed &= Expect(Geometry::CornerClearance(0.0f) == 0.0f, "collapsed boxes do not reserve corner space");

    const Geometry::ClosedBoxFooter child{ 400.0f, 404.0f, 26.0f };
    passed &= Expect(Geometry::FollowsClosedBox(child, 400.0f, 404.0f),
        "adjacent sibling boxes can share a border across separate render helpers");
    passed &= Expect(Geometry::FollowsClosedBox(child, 400.005f, 404.005f),
        "fractional layout rounding does not separate adjacent boxes");
    passed &= Expect(!Geometry::FollowsClosedBox(child, 430.0f, 434.0f),
        "intervening controls prevent sibling boxes from merging");
    passed &= Expect(!Geometry::FollowsClosedBox(child, 400.0f, 414.0f),
        "intentional spacing between boxes is retained");
    passed &= Expect(!Geometry::FollowsClosedBox(std::nullopt, 400.0f, 404.0f),
        "the first box never joins a box from another parent");
    const auto bottomInset = Geometry::NestedBottomInset(child, 400.0f, 404.0f, Geometry::AccentWidth);
    passed &= Expect(bottomInset == 16.0f, "bottom spacing subtracts the ten-pixel colored side rail");
    passed &= Expect(bottomInset && Near(*bottomInset - Geometry::BorderWidth,
        child.leftInset - Geometry::BorderWidth - Geometry::AccentWidth),
        "visible bottom and side gaps match inside the colored frame");
    passed &= Expect(Geometry::NestedBottomInset(child, 400.0f, 404.0f, 0.0f) == 26.0f,
        "plain boxes without a colored rail retain their existing spacing");
    const Geometry::ClosedBoxFooter outerChild{ 416.0f, 420.0f, 26.0f };
    passed &= Expect(Geometry::NestedBottomInset(outerChild, 416.0f, 420.0f, Geometry::AccentWidth) == 16.0f,
        "each nesting level reserves its own matching gap without reusing its child's gap");
    passed &= Expect(!Geometry::NestedBottomInset(child, 430.0f, 434.0f, Geometry::AccentWidth),
        "content after a child keeps the normal content footer");
    passed &= Expect(!Geometry::NestedBottomInset(child, 400.0f, 414.0f, Geometry::AccentWidth),
        "explicit spacing after a child is not mistaken for a flush bottom");
    passed &= Expect(!Geometry::NestedBottomInset(std::nullopt, 400.0f, 404.0f, Geometry::AccentWidth),
        "leaf boxes keep their existing corner clearance");
    const Geometry::ClosedBoxFooter collapsedChild{ 400.0f, 404.0f, 26.0f };
    passed &= Expect(Geometry::NestedBottomInset(collapsedChild, 400.0f, 404.0f, Geometry::AccentWidth) == 16.0f,
        "a collapsed child still gets matching space below it");

    for (const auto inset : std::array{ 16.0f, 26.0f, 36.0f })
    {
        const Geometry::ClosedBoxFooter insetChild{ 400.0f, 404.0f, inset };
        const auto footerHeight = inset - Geometry::AccentWidth;
        passed &= Expect(Geometry::NestedBottomInset(insetChild, 400.0f, 404.0f, Geometry::AccentWidth) == footerHeight,
            "bottom spacing follows the actual clear side gap rather than a fixed pixel value");
        for (const auto height : std::array{ 30.0f, 44.75f, 72.0f })
        {
            const auto originalDrop = Geometry::CornerDrop(height, 600.0f);
            for (const auto childDrop : std::array{ 0.0f, Geometry::CollapsedHeaderDrop(height, 600.0f), originalDrop })
            {
                const auto drop = Geometry::CornerDropAboveNestedBox(originalDrop, inset, footerHeight, childDrop);
                const Geometry::Bevel parent{ 0.0f, 500.0f, drop };
                const auto innerTurn = parent.Turn(Geometry::BorderWidth + Geometry::AccentWidth);
                const auto accentYAtChildLeft = innerTurn.y + (inset - innerTurn.x) / Geometry::SlopeRunPerRise;
                passed &= Expect(drop == 0.0f || accentYAtChildLeft >= parent.bottom - footerHeight - childDrop + Geometry::BorderWidth - 0.001f,
                    "parent corners clear the actual nested corner instead of its rectangular bounds");
                passed &= Expect(drop >= 0.0f && drop <= originalDrop, "corner fitting never enlarges or inverts the angle");
            }
        }
    }
    const auto normalDrop = Geometry::CornerDrop(44.75f, 600.0f);
    passed &= Expect(Near(Geometry::CornerDropAboveNestedBox(normalDrop, 26.0f, 16.0f, normalDrop), normalDrop),
        "expanded nested corners keep their existing size with the corrected visible gap");
    passed &= Expect(Near(Geometry::CornerDropAboveNestedBox(normalDrop, 26.0f, 16.0f,
        Geometry::CollapsedHeaderDrop(44.75f, 600.0f)), normalDrop),
        "collapsed nested corners also preserve the normal parent corner size");
    return passed ? 0 : 1;
}
