#include <MenuControlCorners.h>
#include <MenuControlGeometry.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <vector>

namespace MPL::MenuControlCorners
{
    namespace
    {
        namespace Geometry = MenuControlGeometry;

        struct Frame
        {
            ImGuiMCP::ImDrawList* drawList;
            Geometry::Rect bounds;
            int vertexBegin;
            int vertexEnd;
            float drop;
            float borderSize;
            ImGuiMCP::ImU32 borderColor;
            ImGuiMCP::ImVec4 clip;
            bool left = true;
            bool right = true;
        };

        std::vector<Frame> frames;
        struct Capture
        {
            explicit Capture(const Geometry::Corners a_corners, const float a_width = 0.0f)
            {
                auto* drawList = ImGuiMCP::GetWindowDrawList();
                const auto minimum = ImGuiMCP::GetCursorScreenPos();
                const auto height = ImGuiMCP::GetFrameHeight();
                const Geometry::Rect bounds{ { minimum.x, minimum.y },
                    { minimum.x + (a_width > 0.0f ? a_width : ImGuiMCP::CalcItemWidth()), minimum.y + height } };
                frame = { drawList, bounds, drawList->VtxBuffer.Size, 0,
                    Geometry::CornerDrop(bounds, height), ImGuiMCP::GetStyle()->FrameBorderSize,
                    ImGuiMCP::GetColorU32(ImGuiMCP::ImGuiCol_Border), drawList->_CmdHeader.ClipRect,
                    a_corners.left, a_corners.right };
                ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FrameRounding, 0.0f);
            }

            ~Capture()
            {
                frame.vertexEnd = frame.drawList->VtxBuffer.Size;
                frames.push_back(frame);
                ImGuiMCP::PopStyleVar();
            }

            Capture(const Capture&) = delete;
            Capture& operator=(const Capture&) = delete;

        private:
            Frame frame;
        };

        Geometry::Vertex ReadVertex(const ImGuiMCP::ImDrawVert& a_vertex)
        {
            return { { a_vertex.pos.x, a_vertex.pos.y }, { a_vertex.uv.x, a_vertex.uv.y }, a_vertex.col };
        }

        template <class Buffer, class Element>
        void AssignBuffer(Buffer& a_buffer, const std::vector<Element>& a_values)
        {
            const auto size = static_cast<int>(a_values.size());
            if (a_buffer.Capacity < size)
            {
                auto* data = static_cast<Element*>(ImGuiMCP::MemAlloc(a_values.size() * sizeof(Element)));
                if (!data) throw std::bad_alloc();
                ImGuiMCP::MemFree(a_buffer.Data);
                a_buffer.Data = data;
                a_buffer.Capacity = size;
            }
            if (!a_values.empty()) std::memcpy(a_buffer.Data, a_values.data(), a_values.size() * sizeof(Element));
            a_buffer.Size = size;
        }

        bool ClipDrawList(ImGuiMCP::ImDrawList* a_list, const std::vector<const Frame*>& a_frames)
        {
            assert(a_list->_Splitter._Count <= 1);
            if (a_list->_Splitter._Count > 1 || a_list->CmdBuffer.Size == 0) return false;
            static std::vector<ImGuiMCP::ImDrawVert> vertices;
            static std::vector<ImGuiMCP::ImDrawIdx> indices;
            static std::vector<ImGuiMCP::ImDrawCmd> commands;
            vertices.clear();
            indices.clear();
            commands.clear();
            vertices.reserve(static_cast<std::size_t>(a_list->IdxBuffer.Size));
            indices.reserve(static_cast<std::size_t>(a_list->IdxBuffer.Size));
            const auto supportsOffsets = (a_list->Flags & ImGuiMCP::ImDrawListFlags_AllowVtxOffset) != 0;

            for (int commandIndex = 0; commandIndex < a_list->CmdBuffer.Size; ++commandIndex)
            {
                const auto original = a_list->CmdBuffer.Data[commandIndex];
                const auto beginCommand = [&]()
                {
                    auto command = original;
                    command.IdxOffset = static_cast<unsigned int>(indices.size());
                    command.VtxOffset = supportsOffsets ? static_cast<unsigned int>(vertices.size()) : 0;
                    command.ElemCount = 0;
                    commands.push_back(command);
                };
                beginCommand();
                if (original.UserCallback) continue;
                assert(original.ElemCount % 3 == 0);
                for (unsigned int element = 0; element < original.ElemCount; element += 3)
                {
                    std::array<unsigned int, 3> sourceIndices{};
                    Geometry::Polygon polygon;
                    polygon.count = 3;
                    for (unsigned int corner = 0; corner < 3; ++corner)
                    {
                        const auto source = original.VtxOffset +
                                            a_list->IdxBuffer.Data[original.IdxOffset + element + corner];
                        sourceIndices[corner] = source;
                        polygon.vertices[corner] = ReadVertex(a_list->VtxBuffer.Data[source]);
                    }
                    for (const auto* frame : a_frames)
                    {
                        if (!std::ranges::all_of(sourceIndices, [&](const auto a_index)
                                { return a_index >= static_cast<unsigned int>(frame->vertexBegin) &&
                                         a_index < static_cast<unsigned int>(frame->vertexEnd); }))
                            continue;
                        if (frame->left) polygon = Geometry::ClipCorner(polygon, frame->bounds, frame->drop, true);
                        if (frame->right) polygon = Geometry::ClipCorner(polygon, frame->bounds, frame->drop, false);
                    }
                    for (std::size_t triangle = 1; triangle + 1 < polygon.count; ++triangle)
                    {
                        if (vertices.size() - commands.back().VtxOffset + 3 >
                            std::numeric_limits<ImGuiMCP::ImDrawIdx>::max())
                        {
                            if (!supportsOffsets) return false;
                            beginCommand();
                        }
                        for (const auto corner : std::array<std::size_t, 3>{ 0, triangle, triangle + 1 })
                        {
                            const auto& vertex = polygon.vertices[corner];
                            indices.push_back(static_cast<ImGuiMCP::ImDrawIdx>(vertices.size() - commands.back().VtxOffset));
                            vertices.push_back({
                                ImGuiMCP::ImVec2(vertex.position.x, vertex.position.y),
                                ImGuiMCP::ImVec2(vertex.uv.x, vertex.uv.y), vertex.color });
                            ++commands.back().ElemCount;
                        }
                    }
                }
            }
            AssignBuffer(a_list->VtxBuffer, vertices);
            AssignBuffer(a_list->IdxBuffer, indices);
            AssignBuffer(a_list->CmdBuffer, commands);
            a_list->_VtxWritePtr = a_list->VtxBuffer.Data + a_list->VtxBuffer.Size;
            a_list->_IdxWritePtr = a_list->IdxBuffer.Data + a_list->IdxBuffer.Size;
            a_list->_CmdHeader.VtxOffset = commands.back().VtxOffset;
            a_list->_VtxCurrentIdx = static_cast<unsigned int>(vertices.size()) - commands.back().VtxOffset;
            return true;
        }

        void DrawCornerBorders(const Frame& a_frame)
        {
            if (a_frame.borderSize <= 0.0f) return;
            auto* drawList = a_frame.drawList;
            ImGuiMCP::ImDrawListManager::PushClipRect(drawList,
                ImGuiMCP::ImVec2(a_frame.clip.x, a_frame.clip.y),
                ImGuiMCP::ImVec2(a_frame.clip.z, a_frame.clip.w), false);
            const auto halfBorder = a_frame.borderSize * 0.5f;
            const auto& rect = a_frame.bounds;
            const auto run = a_frame.drop * MenuBoxGeometry::SlopeRunPerRise;
            const auto inset = halfBorder * MenuBoxGeometry::VerticalMiter;
            if (a_frame.left)
                ImGuiMCP::ImDrawListManager::AddLine(drawList,
                    ImGuiMCP::ImVec2(rect.minimum.x + halfBorder, rect.maximum.y - a_frame.drop - inset),
                    ImGuiMCP::ImVec2(rect.minimum.x + run + inset, rect.maximum.y - halfBorder),
                    a_frame.borderColor, a_frame.borderSize);
            if (a_frame.right)
                ImGuiMCP::ImDrawListManager::AddLine(drawList,
                    ImGuiMCP::ImVec2(rect.maximum.x - run - inset, rect.minimum.y + halfBorder),
                    ImGuiMCP::ImVec2(rect.maximum.x - halfBorder, rect.minimum.y + a_frame.drop + inset),
                    a_frame.borderColor, a_frame.borderSize);
            ImGuiMCP::ImDrawListManager::PopClipRect(drawList);
        }

        struct TextPadding
        {
            TextPadding()
            {
                auto padding = ImGuiMCP::GetStyle()->FramePadding;
                const auto height = ImGuiMCP::GetFrameHeight();
                const auto drop = MenuBoxGeometry::CollapsedHeaderDrop(height, height * 4.0f);
                padding.x = std::max(padding.x, drop * MenuBoxGeometry::SlopeRunPerRise * 0.5f);
                ImGuiMCP::PushStyleVar(ImGuiMCP::ImGuiStyleVar_FramePadding, padding);
            }
            ~TextPadding() { ImGuiMCP::PopStyleVar(); }
        };
    }

    Scope::Scope()
    {
        frames.clear();
    }

    Scope::~Scope()
    {
        std::unordered_map<ImGuiMCP::ImDrawList*, std::vector<const Frame*>> drawLists;
        for (const auto& frame : frames)
        {
            if ((frame.left || frame.right) && frame.drop > 0.0f && frame.vertexEnd > frame.vertexBegin)
                drawLists[frame.drawList].push_back(&frame);
        }
        for (const auto& [drawList, targets] : drawLists)
        {
            if (!ClipDrawList(drawList, targets)) continue;
            for (const auto* frame : targets) DrawCornerBorders(*frame);
        }
    }

    bool SliderFloat(const char* a_label, float* a_value, const float a_minimum, const float a_maximum,
        const char* a_format)
    {
        const Capture capture(Geometry::SliderCorners);
        return ImGuiMCP::SliderFloat(a_label, a_value, a_minimum, a_maximum, a_format);
    }

    void CompressionGauge(
        const float a_width,
        const float a_darkLimit,
        const float a_brightLimit)
    {
        const auto width = a_width;
        const Capture capture(Geometry::SliderCorners, width);
        const float height = ImGuiMCP::GetFrameHeight();
        const auto position = ImGuiMCP::GetCursorScreenPos();
        const auto* imguiStyle = ImGuiMCP::GetStyle();
        const float handleThickness = std::clamp(imguiStyle ? imguiStyle->GrabMinSize : 10.0f, 6.0f, height);
        const float frameRounding = imguiStyle ? imguiStyle->FrameRounding : 0.0f;
        const float frameBorderSize = imguiStyle ? imguiStyle->FrameBorderSize : 0.0f;
        ImGuiMCP::Dummy(ImGuiMCP::ImVec2(width, height));

        auto* drawList = ImGuiMCP::GetWindowDrawList();
        const auto frameColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImGuiCol_FrameBg);
        const auto borderColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImGuiCol_Border);
        const ImGuiMCP::ImVec2 minimum(position.x, position.y);
        const ImGuiMCP::ImVec2 maximum(position.x + width, position.y + height);
        const float darkX = position.x + (width * a_darkLimit / 255.0f);
        const float brightX = position.x + (width * a_brightLimit / 255.0f);
        const auto* normalRangeColor = ImGuiMCP::GetStyleColorVec4(ImGuiMCP::ImGuiCol_SliderGrab);
        const auto sliderColor = normalRangeColor ?
                                     *normalRangeColor :
                                     ImGuiMCP::ImVec4(0.24f, 0.52f, 0.88f, 1.0f);
        constexpr float rangeBrightness = 0.7f;
        const auto handColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4(
            sliderColor.x,
            sliderColor.y,
            sliderColor.z,
            sliderColor.w));
        const auto rangeColor = ImGuiMCP::GetColorU32(ImGuiMCP::ImVec4(
            sliderColor.x * rangeBrightness,
            sliderColor.y * rangeBrightness,
            sliderColor.z * rangeBrightness,
            sliderColor.w));
        ImGuiMCP::ImDrawListManager::AddRectFilled(
            drawList,
            minimum,
            maximum,
            frameColor,
            frameRounding,
            0);
        ImGuiMCP::ImDrawListManager::PushClipRect(drawList, minimum, maximum, true);
        ImGuiMCP::ImDrawListManager::AddRectFilled(
            drawList,
            ImGuiMCP::ImVec2(darkX, position.y),
            ImGuiMCP::ImVec2(brightX, position.y + height),
            rangeColor,
            0.0f,
            0);
        ImGuiMCP::ImDrawListManager::AddLine(
            drawList,
            ImGuiMCP::ImVec2(darkX, position.y),
            ImGuiMCP::ImVec2(darkX, position.y + height),
            handColor,
            handleThickness);
        ImGuiMCP::ImDrawListManager::AddLine(
            drawList,
            ImGuiMCP::ImVec2(brightX, position.y),
            ImGuiMCP::ImVec2(brightX, position.y + height),
            handColor,
            handleThickness);
        ImGuiMCP::ImDrawListManager::PopClipRect(drawList);
        if (frameBorderSize > 0.0f)
        {
            ImGuiMCP::ImDrawListManager::AddRect(
                drawList,
                minimum,
                maximum,
                borderColor,
                frameRounding,
                0,
                frameBorderSize);
        }
    }

    bool SliderValueInput(const char* a_label, float* a_value, const char* a_format)
    {
        const TextPadding padding;
        const Capture capture(Geometry::SliderValueCorners);
        return ImGuiMCP::InputFloat(a_label, a_value, 0.0f, 0.0f, a_format);
    }
}
