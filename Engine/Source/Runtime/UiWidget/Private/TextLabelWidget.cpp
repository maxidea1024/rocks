#include <UiWidget/TextLabelWidget.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

TextLabelWidget::TextLabelWidget(Allocator& allocator) noexcept : allocator_(allocator) {
}

Result<void> TextLabelWidget::Shape(TextLabel& label, const Text& value,
                                    float size) const noexcept {
    if (!value.IsValid()) {
        return label.SetShapedText({}, {}, {}, size, false);
    }

    SharedPtr<TextLabel> marker;
    if (ellipsis_ && shaper_) {
        auto shapedMarker = shaper_->ShapeEllipsis(allocator_, size);
        if (!shapedMarker) {
            return Result<void>::Failure(shapedMarker.Error());
        }

        marker = Move(shapedMarker.Value());
    }

    auto shaped =
        shaper_ ? shaper_->Shape(label, value, size) : Result<void>::Failure(ErrorCode::NotReady);
    if (shaped) {
        // 말줄임을 켜면 여러 줄 끝 말줄임이다(Docs/Design/UiToolkit 9.1절 UW4o).
        label.SetOverflow(marker ? TextOverflow::MultiLineEnd : TextOverflow::Clip);
        label.SetEllipsisMarker(Move(marker));
    }

    return shaped;
}

Result<SharedPtr<Element>> TextLabelWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!label) {
        return Result<SharedPtr<Element>>::Failure(label.Error());
    }

    auto layout = ApplyTextLayout(*label.Value());
    if (!layout) {
        return Result<SharedPtr<Element>>::Failure(layout.Error());
    }

    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<SharedPtr<Element>>::Failure(outer.Error());
    }

    auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
    if (!tree) {
        return Result<SharedPtr<Element>>::Failure(tree.Error());
    }

    shaper_ = tree.Value().Get().Value()->TextShaper();
    auto shaped = Shape(*label.Value(), text_, fontSize_);
    if (!shaped) {
        shaper_.Reset();
        return Result<SharedPtr<Element>>::Failure(shaped.Error());
    }

    label.Value()->SetColor(tint_);
    return Result<SharedPtr<Element>>::Success(Move(label.Value()));
}

Result<void> TextLabelWidget::SetText(const Text& text) noexcept {
    auto copy = text.IsValid() ? text.DeepClone(allocator_) : Result<Text>::Success(Text{});
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }

    if (auto live = CachedElement()) {
        auto shaped = Shape(StaticElementCast<TextLabel>(*live), text, fontSize_);
        if (!shaped) {
            return shaped;
        }
    }

    text_ = Move(copy.Value());
    NotifyPropertyChanged("text");
    return Result<void>::Success();
}

const Text& TextLabelWidget::GetText() const noexcept {
    return text_;
}

Result<void> TextLabelWidget::SetFontSize(float size) noexcept {
    if (!math::IsFinite(size) || size <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (fontSize_ != size) {
        if (auto live = CachedElement()) {
            auto shaped = Shape(StaticElementCast<TextLabel>(*live), text_, size);
            if (!shaped) {
                return shaped;
            }
        }

        fontSize_ = size;
        NotifyPropertyChanged("fontSize");
    }

    return Result<void>::Success();
}

Result<void> TextLabelWidget::SetEllipsis(bool enabled) noexcept {
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

        // 말줄임을 켜면 여러 줄 끝 말줄임이다(Docs/Design/UiToolkit 9.1절 UW4o).

        StaticElementCast<TextLabel>(*live).SetOverflow(marker ? TextOverflow::MultiLineEnd
                                                               : TextOverflow::Clip);

        StaticElementCast<TextLabel>(*live).SetEllipsisMarker(Move(marker));
    }

    ellipsis_ = enabled;
    NotifyPropertyChanged("ellipsis");
    return Result<void>::Success();
}

Result<void> TextLabelWidget::SetTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    tint_ = tint;
    if (auto live = CachedElement()) {
        StaticElementCast<TextLabel>(*live).SetColor(tint);
    }

    return Result<void>::Success();
}

Result<void> TextLabelWidget::RefreshText() noexcept {
    auto live = CachedElement();
    return live ? Shape(StaticElementCast<TextLabel>(*live), text_, fontSize_)
                : Result<void>::Success();
}

void TextLabelWidget::ReleaseElements() noexcept {
    Widget::ReleaseElements();
    shaper_.Reset();
}

} // namespace rocks::ui
