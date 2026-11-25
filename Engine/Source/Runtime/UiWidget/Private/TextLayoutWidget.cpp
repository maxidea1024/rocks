#include <UiWidget/TextLayoutWidget.h>

namespace rocks::ui {

TextLabel& TextLayoutWidget::ResolveTextLabel(Element& element) const noexcept {
    return static_cast<TextLabel&>(element);
}

Result<void> TextLayoutWidget::ApplyTextLayout(TextLabel& label) const noexcept {
    auto ready = label.SetLayoutOptions(wrapWidth_, Justification(), autoWrapText_, WrapPolicy());
    if (ready) {
        ready =
            label.SetParagraphStyle({margin_, lineHeightPercentage_, applyLineHeightToBottomLine_},
                                    shadowOffset_, shadowColor_);
    }
    if (ready) {
        ready = label.SetMinDesiredWidth(minDesiredWidth_);
    }
    if (ready) {
        label.SetDecoration(underline_, strikeThrough_);
        label.SetClipToBounds(clipText_);
    }
    return ready;
}

Result<void> TextLayoutWidget::SetLayout(float width, text::TextAlignment alignment, bool automatic,
                                         text::TextWrapPolicy policy) noexcept {
    if (!math::IsFinite(width) || width < 0 ||
        (policy != text::TextWrapPolicy::WordOrCluster && policy != text::TextWrapPolicy::Word) ||
        (alignment != text::TextAlignment::Left && alignment != text::TextAlignment::Center &&
         alignment != text::TextAlignment::Right)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto set = ResolveTextLabel(*live).SetLayoutOptions(width, alignment, automatic, policy);
        if (!set) {
            return set;
        }
    }
    wrapWidth_ = width;
    justification_ = static_cast<uint8>(alignment);
    autoWrapText_ = automatic;
    wrapPolicy_ = static_cast<uint8>(policy);
    return Result<void>::Success();
}

Result<void> TextLayoutWidget::SetWrapWidth(float width) noexcept {
    return SetLayout(width, Justification(), autoWrapText_, WrapPolicy());
}

float TextLayoutWidget::WrapWidth() const noexcept {
    return wrapWidth_;
}

Result<void> TextLayoutWidget::SetAutoWrapText(bool enabled) noexcept {
    return SetLayout(wrapWidth_, Justification(), enabled, WrapPolicy());
}

bool TextLayoutWidget::AutoWrapText() const noexcept {
    return autoWrapText_;
}

Result<void> TextLayoutWidget::SetWrapPolicy(text::TextWrapPolicy policy) noexcept {
    return SetLayout(wrapWidth_, Justification(), autoWrapText_, policy);
}

text::TextWrapPolicy TextLayoutWidget::WrapPolicy() const noexcept {
    return static_cast<text::TextWrapPolicy>(wrapPolicy_);
}

Result<void> TextLayoutWidget::SetJustification(text::TextAlignment alignment) noexcept {
    return SetLayout(wrapWidth_, alignment, autoWrapText_, WrapPolicy());
}

text::TextAlignment TextLayoutWidget::Justification() const noexcept {
    return static_cast<text::TextAlignment>(justification_);
}

Result<void> TextLayoutWidget::SetParagraph(const text::ParagraphMetrics& metrics) noexcept {
    if (!metrics.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto applied =
            ResolveTextLabel(*live).SetParagraphStyle(metrics, shadowOffset_, shadowColor_);
        if (!applied) {
            return applied;
        }
    }
    margin_ = metrics.margin;
    lineHeightPercentage_ = metrics.lineHeightScale;
    applyLineHeightToBottomLine_ = metrics.scaleLastLine;
    return Result<void>::Success();
}

Result<void> TextLayoutWidget::SetMargin(Vec4 margin) noexcept {
    return SetParagraph({margin, lineHeightPercentage_, applyLineHeightToBottomLine_});
}

Result<void> TextLayoutWidget::SetMinDesiredWidth(float width) noexcept {
    if (!math::IsFinite(width) || width < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto set = ResolveTextLabel(*live).SetMinDesiredWidth(width);
        if (!set) {
            return set;
        }
    }
    minDesiredWidth_ = width;
    return Result<void>::Success();
}

Vec4 TextLayoutWidget::Margin() const noexcept {
    return margin_;
}

Result<void> TextLayoutWidget::SetLineHeightPercentage(float scale) noexcept {
    return SetParagraph({margin_, scale, applyLineHeightToBottomLine_});
}

Result<void> TextLayoutWidget::SetApplyLineHeightToBottomLine(bool enabled) noexcept {
    return SetParagraph({margin_, lineHeightPercentage_, enabled});
}

Result<void> TextLayoutWidget::SetTextShadow(Vec2 offset, LinearColor color) noexcept {
    if (!offset.IsFinite() || !color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto live = CachedElement()) {
        auto applied = ResolveTextLabel(*live).SetParagraphStyle(
            {margin_, lineHeightPercentage_, applyLineHeightToBottomLine_}, offset, color);
        if (!applied) {
            return applied;
        }
    }
    shadowOffset_ = offset;
    shadowColor_ = color;
    return Result<void>::Success();
}

void TextLayoutWidget::SetTextDecoration(bool underline, bool strikeThrough) noexcept {
    underline_ = underline;
    strikeThrough_ = strikeThrough;
    if (auto live = CachedElement()) {
        ResolveTextLabel(*live).SetDecoration(underline, strikeThrough);
    }
}

void TextLayoutWidget::SetClipText(bool enabled) noexcept {
    clipText_ = enabled;
    if (auto live = CachedElement()) {
        ResolveTextLabel(*live).SetClipToBounds(enabled);
    }
}

} // namespace rocks::ui
