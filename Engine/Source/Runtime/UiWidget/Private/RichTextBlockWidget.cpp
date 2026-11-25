#include "RichTextMarkup.h"

#include <UiWidget/RichTextBlockWidget.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

RichTextBlockWidget::RichTextBlockWidget(Allocator& allocator) noexcept
    : styles_(allocator), allocator_(allocator) {
}

Result<void> RichTextBlockWidget::Shape(TextLabel& target, const Text& value, float size,
                                        Span<const RichTextStyle> styles,
                                        RichTextDecorator* decorator) const noexcept {
    if (!value.IsValid()) {
        return target.SetShapedText({}, {}, {}, size, false);
    }
    if (!shaper_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    TextPersistentView source;
    TextLabel localized(allocator_);
    auto persisted = value.TryGetPersistentForm(source);
    StringView markup;
    if (persisted && source.form == TextPersistentForm::Invariant) {
        markup = source.source;
    } else {
        // 셰이퍼가 선택한 현재 로캘의 마크업을 파싱한다. 번역문은 원문과 태그 순서가 다를 수 있다.
        auto shaped = shaper_->Shape(localized, value, size);
        if (!shaped) {
            return shaped;
        }
        markup = localized.GetText();
    }

    detail::RichTextMarkup parsed(allocator_);
    auto decoded = detail::ParseRichTextMarkup(markup, styles, decorator, size, parsed);
    if (!decoded) {
        return decoded;
    }
    const auto plain = parsed.plain.View();
    if (plain.IsEmpty()) {
        return target.SetShapedText({}, {}, {}, size, false);
    }
    auto invariant = Text::Invariant(allocator_, plain);
    if (!invariant) {
        return Result<void>::Failure(invariant.Error());
    }
    TextLabel shaped(allocator_);
    auto ready = shaper_->Shape(shaped, invariant.Value(), size);
    if (!ready) {
        return ready;
    }
    if (shaped.GetText() != plain) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Array<text::TextCluster> clusters(allocator_);
    Array<LabelGlyph> glyphs(allocator_);
    auto copied = clusters.TryAppend(shaped.ShapedClusters());
    if (!copied) {
        return copied;
    }
    Array<LabelDecoration> decorations(allocator_);
    bool hasDecoration = false;
    for (const auto& run : parsed.runs) {
        if (run.style && (run.style->underline || run.style->strikeThrough)) {
            hasDecoration = true;
            break;
        }
    }
    if (hasDecoration) {
        auto stored = decorations.TryResize(clusters.Size());
        if (!stored) {
            return stored;
        }
    }
    const auto original = shaped.ShapedGlyphs();
    usize runIndex = 0;
    usize glyphIndex = 0;
    usize begin = 0;
    float height = shaped.LineHeight();
    for (usize i = 0; i < clusters.Size(); ++i) {
        while (parsed.runs[runIndex].end <= begin) {
            ++runIndex;
        }
        auto& cluster = clusters[i];
        const auto& run = parsed.runs[runIndex];
        const auto tint = run.style ? run.style->tint : LinearColor{1, 1, 1, 1};
        if (hasDecoration && run.style) {
            decorations[i] = {tint, run.style->underline, run.style->strikeThrough};
        }
        if (run.embedded) {
            const usize start = runIndex == 0 ? 0 : parsed.runs[runIndex - 1].end;
            if (start != begin || run.end != cluster.byteEnd) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }
            auto glyph = run.image;
            glyph.tint = glyph.tint * tint;
            auto added = glyphs.TryPushBack({i, glyph});
            if (!added) {
                return added;
            }
            cluster.advance = glyph.size.x;
            height = math::Max(height, glyph.size.y + glyph.position.y);
        }
        while (glyphIndex < original.Size() && original[glyphIndex].clusterIndex == i) {
            auto glyph = original[glyphIndex++];
            if (!run.embedded) {
                glyph.glyph.tint = glyph.glyph.tint * tint;
                auto added = glyphs.TryPushBack(glyph);
                if (!added) {
                    return added;
                }
            }
        }
        // 글자와 대체 문자가 하나의 클러스터로 묶인 경우 아이콘 일부만 표시하지 않는다.
        while (parsed.runs[runIndex].end < cluster.byteEnd) {
            if (parsed.runs[++runIndex].embedded) {
                return Result<void>::Failure(ErrorCode::Unsupported);
            }
        }
        begin = cluster.byteEnd;
    }
    SharedPtr<TextLabel> marker;
    if (ellipsis_ && shaper_) {
        auto shapedMarker = shaper_->ShapeEllipsis(allocator_, size);
        if (!shapedMarker) {
            return Result<void>::Failure(shapedMarker.Error());
        }
        marker = Move(shapedMarker.Value());
    }
    auto published = target.SetShapedText(plain, clusters.View(), glyphs.View(), height,
                                          shaped.IsRightToLeft(), decorations.View());
    if (published) {
        target.SetEllipsisMarker(Move(marker));
    }
    return published;
}

Result<SharedPtr<Element>> RichTextBlockWidget::BuildElement(ObjectRegistry& objects) noexcept {
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<SharedPtr<Element>>::Failure(outer.Error());
    }
    auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
    if (!tree) {
        return Result<SharedPtr<Element>>::Failure(tree.Error());
    }
    auto label = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
    if (!label) {
        return Result<SharedPtr<Element>>::Failure(label.Error());
    }
    auto width = ApplyTextLayout(*label.Value());
    if (!width) {
        return Result<SharedPtr<Element>>::Failure(width.Error());
    }
    shaper_ = tree.Value().Get().Value()->TextShaper();
    auto shaped = Shape(*label.Value(), text_, fontSize_, styles_.View(), decorator_.Get());
    if (!shaped) {
        shaper_.Reset();
        return Result<SharedPtr<Element>>::Failure(shaped.Error());
    }
    label.Value()->SetColor(tint_);
    return Result<SharedPtr<Element>>::Success(Move(label.Value()));
}

const Text& RichTextBlockWidget::GetText() const noexcept {
    return text_;
}

Result<void> RichTextBlockWidget::SetText(const Text& value) noexcept {
    auto copy = value.IsValid() ? value.DeepClone(allocator_) : Result<Text>::Success(Text{});
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }
    if (auto live = CachedElement()) {
        auto shaped = Shape(static_cast<TextLabel&>(*live), value, fontSize_, styles_.View(),
                            decorator_.Get());
        if (!shaped) {
            return shaped;
        }
    }
    text_ = Move(copy.Value());
    return Result<void>::Success();
}

Result<void> RichTextBlockWidget::SetStyles(Span<const RichTextStyle> styles) noexcept {
    Array<RichTextStyle> replacement(allocator_);
    for (const auto& style : styles) {
        if (style.name.View().IsEmpty() || !style.tint.IsFinite()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        for (const auto& previous : replacement) {
            if (previous.name.View() == style.name.View()) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }
        }
        auto entry = replacement.TryEmplaceBack(allocator_);
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }
        auto copied = entry.Value()->name.TryAssign(style.name.View());
        if (!copied) {
            return copied;
        }
        entry.Value()->tint = style.tint;
        entry.Value()->underline = style.underline;
        entry.Value()->strikeThrough = style.strikeThrough;
    }
    if (auto live = CachedElement()) {
        auto shaped = Shape(static_cast<TextLabel&>(*live), text_, fontSize_, replacement.View(),
                            decorator_.Get());
        if (!shaped) {
            return shaped;
        }
    }
    styles_ = Move(replacement);
    return Result<void>::Success();
}

Result<void>
RichTextBlockWidget::SetDecorator(const SharedPtr<RichTextDecorator>& decorator) noexcept {
    if (auto live = CachedElement()) {
        auto shaped = Shape(static_cast<TextLabel&>(*live), text_, fontSize_, styles_.View(),
                            decorator.Get());
        if (!shaped) {
            return shaped;
        }
    }
    decorator_ = decorator;
    return Result<void>::Success();
}

Result<void> RichTextBlockWidget::SetFontSize(float size) noexcept {
    if (!math::IsFinite(size) || size <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement(); live && size != fontSize_) {
        auto shaped =
            Shape(static_cast<TextLabel&>(*live), text_, size, styles_.View(), decorator_.Get());
        if (!shaped) {
            return shaped;
        }
    }
    fontSize_ = size;
    return Result<void>::Success();
}

Result<void> RichTextBlockWidget::SetEllipsis(bool enabled) noexcept {
    if (enabled == ellipsis_) {
        return Result<void>::Success();
    }
    if (auto live = CachedElement()) {
        SharedPtr<TextLabel> marker;
        if (enabled && shaper_) {
            auto shaped = shaper_->ShapeEllipsis(allocator_, fontSize_);
            if (!shaped) {
                return Result<void>::Failure(shaped.Error());
            }
            marker = Move(shaped.Value());
        }
        static_cast<TextLabel&>(*live).SetEllipsisMarker(Move(marker));
    }
    ellipsis_ = enabled;
    NotifyPropertyChanged("ellipsis");
    return Result<void>::Success();
}

Result<void> RichTextBlockWidget::SetTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        static_cast<TextLabel&>(*live).SetColor(tint);
    }
    tint_ = tint;
    return Result<void>::Success();
}

Result<void> RichTextBlockWidget::RefreshText() noexcept {
    auto live = CachedElement();
    return live ? Shape(static_cast<TextLabel&>(*live), text_, fontSize_, styles_.View(),
                        decorator_.Get())
                : Result<void>::Success();
}

void RichTextBlockWidget::ReleaseElements() noexcept {
    Widget::ReleaseElements();
    shaper_.Reset();
}

} // namespace rocks::ui
