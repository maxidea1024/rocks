#include <UiFramework/TextLabel.h>

namespace rocks::ui {

Result<void> TextLabel::PaintOverflow(PaintList& list, int32 layer) const noexcept {
    const auto& area = GetGeometry();
    const Vec2 shadow = shadowColor_.a > 0 && !glyphs_.IsEmpty() ? shadowOffset_ : Vec2{};
    const float left = metrics_.margin.x + math::Max(0.0f, -shadow.x);
    const float width = area.size.x - left - metrics_.margin.z - math::Max(0.0f, shadow.x);
    const float bottom = area.size.y - metrics_.margin.w - math::Max(0.0f, shadow.y);
    const auto lines = layout_.Lines();
    if (width <= 0 || lines.IsEmpty() || lines[0].y >= bottom) {
        return Result<void>::Success();
    }
    // 첫 줄이 일부만 보여도 표시하되, 다음 줄이 반쯤 보일 때는 마지막 온전한 줄에서 생략한다.
    usize visible = 1;
    while (visible < lines.Size() && lines[visible].y + lines[visible].height <= bottom) {
        ++visible;
    }

    const auto visual = layout_.VisualClusters();
    const float markerWidth = ellipsisMarker_->layout_.DesiredSize().x;
    const auto pass = [&](bool isShadow) noexcept -> Result<void> {
        const Vec2 offset = isShadow ? shadowOffset_ : Vec2{};
        const LinearColor tint = isShadow ? LinearColor(shadowColor_.r, shadowColor_.g,
                                                        shadowColor_.b, shadowColor_.a * color_.a)
                                          : color_;
        for (usize row = 0; row < visible; ++row) {
            const auto& line = lines[row];
            const bool truncate =
                line.width > width || (row + 1 == visible && visible < lines.Size());
            usize first = 0;
            usize count = line.visualCount;
            float shift = 0;
            float markerX = 0;
            if (truncate) {
                if (markerWidth > width) {
                    continue;
                }
                float kept = 0;
                count = 0;
                while (count < line.visualCount) {
                    const usize i = rightToLeft_ ? line.visualCount - 1 - count : count;
                    const float next = kept + visual[line.firstVisual + i].advance;
                    if (next > width - markerWidth) {
                        break;
                    }
                    kept = next;
                    ++count;
                }
                first = rightToLeft_ ? line.visualCount - count : 0;
                const float spare = math::Max(0.0f, width - (kept + markerWidth));
                float anchor = left;
                if (alignment_ == text::TextAlignment::Right) {
                    anchor += spare;
                } else if (alignment_ == text::TextAlignment::Center) {
                    anchor += spare * 0.5f;
                }
                markerX = rightToLeft_ ? anchor : anchor + kept;
                if (count != 0) {
                    shift = anchor + (rightToLeft_ ? markerWidth : 0) -
                            visual[line.firstVisual + first].x;
                }
            }
            auto content =
                area.MakeChild(area.size, {1, 0, 0, 1, shift + offset.x, offset.y}, {}, {});
            if (count != 0 && !visualGlyphStarts_.IsEmpty()) {
                const usize begin = visualGlyphStarts_[line.firstVisual + first];
                const usize end = visualGlyphStarts_[line.firstVisual + first + count];
                auto drawn = list.TryAddGlyphRun(content, {positioned_.Data() + begin, end - begin},
                                                 tint, layer, !isShadow);
                if (!drawn) {
                    return drawn;
                }
            }
            if (truncate) {
                auto marker = area.MakeChild(
                    area.size, {1, 0, 0, 1, markerX + offset.x, line.y + offset.y}, {}, {});
                auto drawn = list.TryAddGlyphRun(marker, ellipsisMarker_->positioned_.View(), tint,
                                                 layer, !isShadow);
                if (!drawn) {
                    return drawn;
                }
            }
            if (!isShadow) {
                auto decorated = PaintDecorations(list, content, layer, line, first, count);
                if (!decorated) {
                    return decorated;
                }
            }
        }
        return Result<void>::Success();
    };

    auto painted = list.PushClip(area);
    if (!painted) {
        return painted;
    }
    if (shadow != Vec2{}) {
        painted = pass(true);
    }
    if (painted) {
        painted = pass(false);
    }
    list.PopClip();
    return painted;
}

} // namespace rocks::ui
