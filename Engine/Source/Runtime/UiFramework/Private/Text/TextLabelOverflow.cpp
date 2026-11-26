#include <UiFramework/Popups/ToolTip.h>
#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

namespace {

// 받은 영역과 원하는 크기를 비교할 때 무시하는 오차(px)다 — 배치 반올림으로 생기는 소수 차이를
// 넘침으로 보지 않는다.
constexpr float OverflowTolerance = 0.01f;

// 한 줄을 자른 결과다: 시각 순서 왼쪽 앞부분 leftCount개와 오른쪽 뒷부분 rightCount개를 남기고 그
// 사이에 표지를 둔다(둘 중 하나는 0일 수 있다).
struct LineCut {
    usize leftCount = 0;
    usize rightCount = 0;
    float leftWidth = 0;
    float rightWidth = 0;
};

enum class CutSide : uint8 {
    Left,
    Right,
    Both
};

// 시각 클러스터 advance 목록에서 available 폭 안에 남길 앞·뒤를 고른다. Both는 남긴 폭이 작은
// 쪽부터 번갈아 더한다(가운데 말줄임).
LineCut CutLine(Span<const text::VisualCluster> visual, float available, CutSide side) noexcept {
    LineCut cut;
    const usize count = visual.Size();
    if (side == CutSide::Left) {
        while (cut.leftCount < count &&
               cut.leftWidth + visual[cut.leftCount].advance <= available) {
            cut.leftWidth += visual[cut.leftCount++].advance;
        }

        return cut;
    }

    if (side == CutSide::Right) {
        while (cut.rightCount < count &&
               cut.rightWidth + visual[count - 1 - cut.rightCount].advance <= available) {
            cut.rightWidth += visual[count - 1 - cut.rightCount++].advance;
        }

        return cut;
    }

    while (cut.leftCount + cut.rightCount < count) {
        const float nextLeft = visual[cut.leftCount].advance;
        const float nextRight = visual[count - 1 - cut.rightCount].advance;
        const float used = cut.leftWidth + cut.rightWidth;
        const bool leftFits = used + nextLeft <= available;
        const bool rightFits = used + nextRight <= available;
        if (!leftFits && !rightFits) {
            break;
        }

        // 남긴 폭이 작은 쪽(같으면 앞)을 늘린다. 그쪽이 안 들어가면 다른 쪽.
        const bool preferLeft = cut.leftWidth <= cut.rightWidth;
        if ((preferLeft && leftFits) || !rightFits) {
            cut.leftWidth += nextLeft;
            ++cut.leftCount;
        } else {
            cut.rightWidth += nextRight;
            ++cut.rightCount;
        }
    }

    return cut;
}

} // namespace

void TextLabel::SetOverflow(TextOverflow overflow) noexcept {
    if (overflow_ != overflow) {
        overflow_ = overflow;
        InvalidatePaint();
    }
}

TextOverflow TextLabel::GetOverflow() const noexcept {
    return overflow_;
}

bool TextLabel::IsTruncated() const noexcept {
    const Vec2 desired = layout_.DesiredSize();
    const Vec2 size = GetGeometry().size;
    return desired.x > size.x + OverflowTolerance || desired.y > size.y + OverflowTolerance;
}

Result<void> TextLabel::SetFullTextToolTip(bool enabled) noexcept {
    if (enabled == fullTextToolTip_) {
        return Result<void>::Success();
    }

    auto set = SetToolTipFactory(
        enabled ? ToolTipFactory::CreateLambda([this]() noexcept { return MakeFullTextToolTip(); })
                : ToolTipFactory{});
    if (set) {
        fullTextToolTip_ = enabled;
    }

    return set;
}

Result<SharedPtr<Element>> TextLabel::MakeFullTextToolTip() noexcept {
    // 잘리지 않았으면 툴팁이 없다(Application은 실패한 factory를 띄우지 않는다).
    if (!IsTruncated()) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::NotFound);
    }

    // 이미 shaping한 원문·클러스터·글리프를 복사해 툴팁 줄바꿈 폭으로 다시 나눈다.
    auto copy = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator_, allocator_);
    if (!copy || !tip) {
        return Result<SharedPtr<Element>>::Failure(!copy ? copy.Error() : tip.Error());
    }

    auto made = copy.Value()->SetShapedText(source_.View(), clusters_.View(), glyphs_.View(),
                                            lineHeight_, rightToLeft_, decorations_.View());
    if (made) {
        made = copy.Value()->SetLayoutOptions(0, alignment_, true, policy_);
    }

    if (made) {
        made = tip.Value()->Construct({.text = Move(copy.Value())});
    }

    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    return Result<SharedPtr<Element>>::Success(Move(tip.Value()));
}

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
    const LinearColor color = color_.Resolve(list.Theme(), list.ColorContext());
    // 폭을 넘는 줄의 자르는 쪽이다(시각 순서). 논리 끝은 LTR이면 오른쪽, RTL이면 왼쪽이다.
    const CutSide wideSide = overflow_ == TextOverflow::Middle                    ? CutSide::Both
                             : (overflow_ == TextOverflow::Start) != rightToLeft_ ? CutSide::Right
                                                                                  : CutSide::Left;
    // 높이를 넘을 때 마지막 줄은 늘 논리 끝(숨은 글자가 이어지는 쪽)에 표지를 둔다.
    const CutSide lastSide = rightToLeft_ ? CutSide::Right : CutSide::Left;
    const bool markHidden = overflow_ != TextOverflow::End && visible < lines.Size();
    const auto pass = [&](bool isShadow) noexcept -> Result<void> {
        const Vec2 offset = isShadow ? shadowOffset_ : Vec2{};
        const LinearColor tint = isShadow ? LinearColor(shadowColor_.r, shadowColor_.g,
                                                        shadowColor_.b, shadowColor_.a * color.a)
                                          : color;
        // 시각 구간 [first, first + count)를 x에서 시작하게 옮겨 그린다.
        const auto drawRun = [&](const text::TextLine& line, usize first, usize count,
                                 float shift) noexcept -> Result<void> {
            if (count == 0 || visualGlyphStarts_.IsEmpty()) {
                return Result<void>::Success();
            }

            auto content =
                area.MakeChild(area.size, {1, 0, 0, 1, shift + offset.x, offset.y}, {}, {});
            const usize begin = visualGlyphStarts_[line.firstVisual + first];
            const usize end = visualGlyphStarts_[line.firstVisual + first + count];
            auto drawn = list.TryAddGlyphRun(content, {positioned_.Data() + begin, end - begin},
                                             tint, layer, !isShadow);
            if (drawn && !isShadow) {
                drawn = PaintDecorations(list, content, layer, line, first, count);
            }

            return drawn;
        };
        for (usize row = 0; row < visible; ++row) {
            const auto& line = lines[row];
            const bool wide = line.width > width;
            const bool hiddenAfter = markHidden && row + 1 == visible;
            if (!wide && !hiddenAfter) {
                auto drawn = drawRun(line, 0, line.visualCount, 0);
                if (!drawn) {
                    return drawn;
                }

                continue;
            }

            if (markerWidth > width) {
                continue;
            }

            const Span<const text::VisualCluster> clusters{visual.Data() + line.firstVisual,
                                                           line.visualCount};
            const LineCut cut =
                CutLine(clusters, width - markerWidth, hiddenAfter ? lastSide : wideSide);
            const float total = cut.leftWidth + markerWidth + cut.rightWidth;
            const float spare = math::Max(0.0f, width - total);
            float anchor = left;
            if (alignment_ == text::TextAlignment::Right) {
                anchor += spare;
            } else if (alignment_ == text::TextAlignment::Center) {
                anchor += spare * 0.5f;
            }

            if (cut.leftCount != 0) {
                auto drawn = drawRun(line, 0, cut.leftCount, anchor - clusters[0].x);
                if (!drawn) {
                    return drawn;
                }
            }

            const float markerX = anchor + cut.leftWidth;
            if (cut.rightCount != 0) {
                const usize first = line.visualCount - cut.rightCount;
                auto drawn =
                    drawRun(line, first, cut.rightCount, markerX + markerWidth - clusters[first].x);
                if (!drawn) {
                    return drawn;
                }
            }

            auto marker = area.MakeChild(
                area.size, {1, 0, 0, 1, markerX + offset.x, line.y + offset.y}, {}, {});
            auto drawn = list.TryAddGlyphRun(marker, ellipsisMarker_->positioned_.View(), tint,
                                             layer, !isShadow);
            if (!drawn) {
                return drawn;
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
