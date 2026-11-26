#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

namespace {

float WrapLimit(float requested, bool automatic, float allotted) noexcept {
    if (!automatic || allotted == 0) {
        return requested;
    }

    return requested == 0 ? allotted : math::Min(requested, allotted);
}

Vec2 ShadowExtent(Vec2 offset, LinearColor color) noexcept {
    return color.a > 0 ? offset : Vec2{};
}

} // namespace

TextLabel::TextLabel(Allocator& allocator) noexcept
    : Element(allocator), allocator_(allocator), source_(allocator), clusters_(allocator),
      glyphs_(allocator), layout_(allocator), positioned_(allocator), visualGlyphStarts_(allocator),
      decorations_(allocator) {
    SetVisibility(Visibility::SelfHitTestInvisible);
}

Result<void> TextLabel::SetShapedText(StringView source, Span<const text::TextCluster> clusters,
                                      Span<const LabelGlyph> glyphs, float lineHeight,
                                      bool rightToLeft,
                                      Span<const LabelDecoration> decorations) noexcept {
    if (!decorations.IsEmpty() && decorations.Size() != clusters.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const auto& decor : decorations) {
        if (!decor.color.IsFinite()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    auto valid = source.ValidateUtf8();
    if (!valid) {
        return valid;
    }

    if ((clusters.IsEmpty() ? 0 : clusters[clusters.Size() - 1].byteEnd) != source.ByteCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const auto& cluster : clusters) {
        if (cluster.byteEnd > source.ByteCount() ||
            (cluster.byteEnd < source.ByteCount() &&
             (static_cast<uint8>(source.Data()[cluster.byteEnd]) & 0xc0) == 0x80)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    for (usize i = 0; i < glyphs.Size(); ++i) {
        const auto& glyph = glyphs[i];
        if (glyph.clusterIndex >= clusters.Size() || clusters[glyph.clusterIndex].hardBreak ||
            (i && glyphs[i - 1].clusterIndex > glyph.clusterIndex) ||
            !glyph.glyph.tint.IsFinite() || !glyph.glyph.position.IsFinite() ||
            !glyph.glyph.size.IsFinite() || glyph.glyph.size.x < 0 || glyph.glyph.size.y < 0 ||
            !glyph.glyph.uv.IsValid() ||
            (glyph.glyph.size.x > 0 && glyph.glyph.size.y > 0 && glyph.glyph.resource.IsNull())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    String string(allocator_);
    Array<text::TextCluster> newClusters(allocator_);
    Array<LabelGlyph> newGlyphs(allocator_);
    auto stored = string.TryAssign(source);
    if (!stored) {
        return stored;
    }

    stored = newClusters.TryAppend(clusters);
    if (!stored) {
        return stored;
    }

    stored = newGlyphs.TryAppend(glyphs);
    if (!stored) {
        return stored;
    }

    Array<LabelDecoration> newDecorations(allocator_);
    stored = newDecorations.TryAppend(decorations);
    if (!stored) {
        return stored;
    }

    auto arranged = Reflow(clusters, glyphs, WrapLimit(wrapWidth_, autoWrap_, allottedWidth_),
                           lineHeight, alignment_, rightToLeft, allottedWidth_, policy_, metrics_,
                           ShadowExtent(shadowOffset_, shadowColor_));
    if (!arranged) {
        return arranged;
    }

    source_ = Move(string);
    clusters_ = Move(newClusters);
    glyphs_ = Move(newGlyphs);
    decorations_ = Move(newDecorations);
    lineHeight_ = lineHeight;
    rightToLeft_ = rightToLeft;
    InvalidateLayout();
    return Result<void>::Success();
}

Result<void> TextLabel::Reflow(Span<const text::TextCluster> clusters,
                               Span<const LabelGlyph> glyphs, float width, float height,
                               text::TextAlignment alignment, bool rtl, float alignmentWidth,
                               text::TextWrapPolicy policy, const text::ParagraphMetrics& metrics,
                               Vec2 shadowExtent) noexcept {
    auto paragraph = metrics;
    if (!glyphs.IsEmpty()) {
        // 음수 그림자는 앞쪽 여백을, 양수 그림자는 뒤쪽 여백을 차지한다.
        paragraph.margin = paragraph.margin +
                           Vec4(math::Max(0.0f, -shadowExtent.x), math::Max(0.0f, -shadowExtent.y),
                                math::Max(0.0f, shadowExtent.x), math::Max(0.0f, shadowExtent.y));
    }

    text::TextLayoutModel layout(allocator_);
    auto shaped =
        layout.Layout(clusters, width, height, alignment, rtl, alignmentWidth, policy, paragraph);
    if (!shaped) {
        return shaped;
    }

    Array<usize> starts(allocator_);
    if (clusters.Size() == MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto sized = starts.TryResize(clusters.Size() + 1);
    if (!sized) {
        return sized;
    }

    usize next = 0;
    for (usize i = 0; i < clusters.Size(); ++i) {
        starts[i] = next;
        while (next < glyphs.Size() && glyphs[next].clusterIndex == i) {
            ++next;
        }
    }

    starts[clusters.Size()] = next;

    Array<PaintGlyph> positioned(allocator_);
    auto reserved = positioned.TryReserve(glyphs.Size());
    if (!reserved) {
        return reserved;
    }

    const auto visual = layout.VisualClusters();
    Array<usize> visualStarts(allocator_);
    if (!glyphs.IsEmpty()) {
        sized = visualStarts.TryResize(visual.Size() + 1);
        if (!sized) {
            return sized;
        }

        visualStarts[visual.Size()] = glyphs.Size();
    }

    const auto lines = layout.Lines();
    for (usize row = 0; row < lines.Size(); ++row) {
        const auto& line = lines[row];
        for (usize i = 0; i < line.visualCount; ++i) {
            if (!visualStarts.IsEmpty()) {
                visualStarts[line.firstVisual + i] = positioned.Size();
            }

            const auto& cluster = visual[line.firstVisual + i];
            const Vec2 origin(cluster.x, line.y);
            for (usize j = starts[cluster.logicalIndex]; j < starts[cluster.logicalIndex + 1];
                 ++j) {
                PaintGlyph glyph = glyphs[j].glyph;
                glyph.position += origin;
                if (!glyph.position.IsFinite()) {
                    return Result<void>::Failure(ErrorCode::Overflow);
                }

                auto added = positioned.TryPushBack(glyph);
                if (!added) {
                    return added;
                }
            }
        }
    }

    layout_ = Move(layout);
    positioned_ = Move(positioned);
    visualGlyphStarts_ = Move(visualStarts);
    return Result<void>::Success();
}

Result<void> TextLabel::SetWrapping(float width, text::TextAlignment alignment) noexcept {
    return SetLayoutOptions(width, alignment, false, policy_);
}

Result<void> TextLabel::SetLayoutOptions(float width, text::TextAlignment alignment, bool autoWrap,
                                         text::TextWrapPolicy policy) noexcept {
    if (!math::IsFinite(width) || width < 0 ||
        (policy != text::TextWrapPolicy::WordOrCluster && policy != text::TextWrapPolicy::Word) ||
        (alignment != text::TextAlignment::Left && alignment != text::TextAlignment::Right &&
         alignment != text::TextAlignment::Center)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (width == wrapWidth_ && alignment == alignment_ && autoWrap == autoWrap_ &&
        policy == policy_) {
        return Result<void>::Success();
    }

    if (lineHeight_ > 0) {
        auto arranged =
            Reflow(clusters_.View(), glyphs_.View(), WrapLimit(width, autoWrap, allottedWidth_),
                   lineHeight_, alignment, rightToLeft_, allottedWidth_, policy, metrics_,
                   ShadowExtent(shadowOffset_, shadowColor_));
        if (!arranged) {
            return arranged;
        }
    }

    wrapWidth_ = width;
    alignment_ = alignment;
    autoWrap_ = autoWrap;
    policy_ = policy;
    InvalidateLayout();
    return Result<void>::Success();
}

Result<void> TextLabel::ArrangeForWidth(float width) noexcept {
    if (!math::IsFinite(width) || width < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const auto limit = WrapLimit(wrapWidth_, autoWrap_, width);
    const bool wrapChanged = limit != WrapLimit(wrapWidth_, autoWrap_, allottedWidth_);
    if (lineHeight_ > 0 && width != allottedWidth_ &&
        (wrapChanged || alignment_ != text::TextAlignment::Left)) {
        const Vec2 previous = layout_.DesiredSize();
        auto arranged =
            Reflow(clusters_.View(), glyphs_.View(), limit, lineHeight_, alignment_, rightToLeft_,
                   width, policy_, metrics_, ShadowExtent(shadowOffset_, shadowColor_));
        if (!arranged) {
            return arranged;
        }

        if (previous != layout_.DesiredSize()) {
            InvalidateLayout();
        } else {
            InvalidatePaint();
        }
    }

    allottedWidth_ = width;
    return Result<void>::Success();
}

Result<void> TextLabel::Arrange(const Geometry& geometry) noexcept {
    auto arranged = ArrangeForWidth(geometry.size.x);
    if (!arranged) {
        return arranged;
    }

    return Element::Arrange(geometry);
}

Result<void> TextLabel::SetParagraphStyle(const text::ParagraphMetrics& metrics, Vec2 offset,
                                          LinearColor color) noexcept {
    if (!metrics.IsValid() || !offset.IsFinite() || !color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec2 extent = ShadowExtent(offset, color);
    const bool layoutChanged =
        metrics_ != metrics || ShadowExtent(shadowOffset_, shadowColor_) != extent;
    if (layoutChanged && lineHeight_ > 0) {
        auto arranged = Reflow(clusters_.View(), glyphs_.View(),
                               WrapLimit(wrapWidth_, autoWrap_, allottedWidth_), lineHeight_,
                               alignment_, rightToLeft_, allottedWidth_, policy_, metrics, extent);
        if (!arranged) {
            return arranged;
        }
    }

    if (layoutChanged) {
        InvalidateLayout();
    } else if (shadowColor_ != color) {
        InvalidatePaint();
    }

    metrics_ = metrics;
    shadowOffset_ = offset;
    shadowColor_ = color;
    return Result<void>::Success();
}

void TextLabel::SetDecoration(bool underline, bool strikeThrough) noexcept {
    if (underline_ != underline || strikeThrough_ != strikeThrough) {
        underline_ = underline;
        strikeThrough_ = strikeThrough;
        InvalidatePaint();
    }
}

float TextLabel::LineStride() const noexcept {
    return lineHeight_ * metrics_.lineHeightScale;
}

Result<void> TextLabel::SetMinDesiredWidth(float width) noexcept {
    if (!math::IsFinite(width) || width < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (width != minDesiredWidth_) {
        minDesiredWidth_ = width;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> TextLabel::CopyParagraphTo(TextLabel& target) const noexcept {
    auto copied = target.SetLayoutOptions(wrapWidth_, alignment_, autoWrap_, policy_);
    if (copied) {
        copied = target.SetParagraphStyle(metrics_, shadowOffset_, shadowColor_);
    }

    return copied;
}

void TextLabel::SetEllipsisMarker(SharedPtr<const TextLabel> marker) noexcept {
    for (const TextLabel* ancestor = marker.Get(); ancestor;
         ancestor = ancestor->ellipsisMarker_.Get()) {
        if (ancestor == this) {
            return;
        }
    }

    if (marker.Get() != ellipsisMarker_.Get()) {
        ellipsisMarker_ = Move(marker);
        InvalidatePaint();
    }
}

void TextLabel::SetColor(LinearColor color) noexcept {
    SetColor(ColorRef::Specified(color));
}

void TextLabel::SetColor(ColorRef color) noexcept {
    if (color.IsValid() && color_ != color) {
        color_ = color;
        InvalidatePaint();
    }
}

ColorRef TextLabel::GetColor() const noexcept {
    return color_;
}

StringView TextLabel::DefaultAccessibleText() const noexcept {
    return GetText();
}

StringView TextLabel::GetText() const noexcept {
    return source_.View();
}

const text::TextLayoutModel& TextLabel::Layout() const noexcept {
    return layout_;
}

Span<const text::TextCluster> TextLabel::ShapedClusters() const noexcept {
    return clusters_.View();
}

Span<const LabelGlyph> TextLabel::ShapedGlyphs() const noexcept {
    return glyphs_.View();
}

float TextLabel::LineHeight() const noexcept {
    return lineHeight_;
}

bool TextLabel::IsRightToLeft() const noexcept {
    return rightToLeft_;
}

usize TextLabel::HitText(Vec2 localPosition) const noexcept {
    return layout_.HitTest(localPosition);
}

Result<Vec2> TextLabel::CaretPosition(usize offset, bool trailing) const noexcept {
    return layout_.CaretPosition(offset, trailing);
}

Result<void> TextLabel::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Vec2 TextLabel::MeasureForWidth(float width) noexcept {
    // 그 폭으로 줄을 다시 나눈 결과의 크기다. 실패하면(폭이 유한하지 않음 등) 지금 크기다.
    (void)ArrangeForWidth(math::Max(0.0f, width));
    return ComputeDesiredSize(1);
}

Vec2 TextLabel::ComputeDesiredSize(float) const noexcept {
    const Vec2 desired = layout_.DesiredSize();
    return {math::Max(minDesiredWidth_, desired.x), desired.y};
}

Result<void> TextLabel::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& area = GetGeometry();
    const LinearColor color = color_.Resolve(list.Theme(), list.ColorContext());
    if (ellipsisMarker_ &&
        (layout_.DesiredSize().x > area.size.x || layout_.DesiredSize().y > area.size.y)) {
        return PaintOverflow(list, layer);
    }

    if (shadowColor_.a > 0 && shadowOffset_ != Vec2{}) {
        const auto geometry =
            area.MakeChild(area.size, {1, 0, 0, 1, shadowOffset_.x, shadowOffset_.y}, {}, {});
        const auto shadowColor =
            LinearColor(shadowColor_.r, shadowColor_.g, shadowColor_.b, shadowColor_.a * color.a);
        auto shadow = list.TryAddGlyphRun(geometry, positioned_.View(), shadowColor, layer, false);
        if (!shadow) {
            return shadow;
        }
    }

    auto painted = list.TryAddGlyphRun(area, positioned_.View(), color, layer);
    if (!painted || (!underline_ && !strikeThrough_ && decorations_.IsEmpty())) {
        return painted;
    }

    for (const auto& line : layout_.Lines()) {
        auto drawn = PaintDecorations(list, area, layer, line, 0, line.visualCount);
        if (!drawn) {
            return drawn;
        }
    }

    return Result<void>::Success();
}

Result<void> TextLabel::PaintDecorations(PaintList& list, const Geometry& area, int32 layer,
                                         const text::TextLine& line, usize firstIndex,
                                         usize count) const noexcept {
    if (!underline_ && !strikeThrough_ && decorations_.IsEmpty()) {
        return Result<void>::Success();
    }

    const float thickness = math::Min(lineHeight_, math::Max(1.0f, lineHeight_ / 16.0f));
    const LinearColor color = color_.Resolve(list.Theme(), list.ColorContext());
    const auto visual = layout_.VisualClusters();
    for (usize begin = firstIndex; begin < firstIndex + count;) {
        const auto& first = visual[line.firstVisual + begin];
        const LabelDecoration decor =
            decorations_.IsEmpty() ? LabelDecoration{} : decorations_[first.logicalIndex];
        usize end = begin + 1;
        while (end < firstIndex + count &&
               (decorations_.IsEmpty() ||
                decorations_[visual[line.firstVisual + end].logicalIndex] == decor)) {
            ++end;
        }

        const auto& last = visual[line.firstVisual + end - 1];
        const float length = last.x + last.advance - first.x;
        auto draw = [&](float y) noexcept {
            const auto box =
                area.MakeChild({length, thickness}, {1, 0, 0, 1, first.x, line.y + y}, {}, {});
            return list.TryAddBox({box, decor.color * color, layer});
        };
        if (length > 0 && (decor.underline || underline_)) {
            auto painted = draw(lineHeight_ - thickness);
            if (!painted) {
                return painted;
            }
        }

        if (length > 0 && (decor.strikeThrough || strikeThrough_)) {
            auto painted = draw((lineHeight_ - thickness) * 0.5f);
            if (!painted) {
                return painted;
            }
        }

        begin = end;
    }

    return Result<void>::Success();
}

AccessibleRole TextLabel::GetAccessibleRole() const noexcept {
    return AccessibleRole::Text;
}

} // namespace rocks::ui
