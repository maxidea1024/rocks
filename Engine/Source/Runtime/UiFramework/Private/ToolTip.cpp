#include <Core/Math/MathUtility.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/HorizontalStack.h>
#include <UiFramework/ToolTip.h>
#include <UiFramework/VerticalStack.h>

namespace rocks::ui {

namespace {

bool IsValidMargin(Vec4 padding) noexcept {
    return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
           padding.w >= 0;
}

bool IsValidLength(float value) noexcept {
    return math::IsFinite(value) && value >= 0;
}

bool IsValidStyle(const ToolTipStyle& style) noexcept {
    return IsValidMargin(style.padding) && IsValidMargin(style.shortcutPadding) &&
           IsValidLength(style.wrapWidth) && IsValidLength(style.shortcutGap) &&
           IsValidLength(style.lineGap);
}

} // namespace

ToolTip::ToolTip(Allocator& allocator) noexcept : Frame(allocator), allocator_(allocator) {
}

Result<void> ToolTip::Construct(Args&& args) noexcept {
    const bool parts = args.title || args.shortcut || args.description;
    const int kinds = (args.text ? 1 : 0) + (parts ? 1 : 0) + (args.content.element ? 1 : 0);
    if (kinds > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const ToolTipStyle& style =
        args.style ? *args.style : (args.text ? DefaultToolTipStyle() : ContentToolTipStyle());
    if (!IsValidStyle(style)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Brush none;
    none.mode = BrushMode::None;
    Result<void> status = Result<void>::Success();
    if (args.text) {
        status = Frame::Construct({.content = args.text, .brush = none});
    } else if (parts) {
        auto built = BuildParts(args, style);
        status = built ? Frame::Construct({.content = built.Value(), .brush = none})
                       : Result<void>::Failure(built.Error());
    } else {
        status = Frame::Construct({.content = Move(args.content), .brush = none});
    }

    if (!status) {
        return status;
    }

    text_ = Move(args.text);
    title_ = Move(args.title);
    description_ = Move(args.description);
    shortcutText_ = Move(args.shortcut);
    return SetStyle(style);
}

Result<SharedPtr<Element>> ToolTip::BuildParts(const Args& args,
                                               const ToolTipStyle& style) noexcept {
    auto column = SharedPtr<VerticalStack>::TryMake(allocator_, allocator_);
    auto header = SharedPtr<HorizontalStack>::TryMake(allocator_, allocator_);
    if (!column || !header) {
        return Result<SharedPtr<Element>>::Failure(!column ? column.Error() : header.Error());
    }

    Result<void> status = Result<void>::Success();
    if (args.title) {
        // 제목은 Fill 칸이다 — 좁은 창에서 남는 폭으로 줄을 나누고 단축키 표지는 오른쪽에 남는다.
        status = header.Value()->TryAddSlot(
            args.title, {.rule = StackSizeRule::Fill, .alignment = StackAlignment::Center});
    }

    if (status && args.shortcut) {
        // 단축키 표지는 바탕·여백만 다른 작은 툴팁 상자다.
        auto chip = SharedPtr<ToolTip>::TryMake(allocator_, allocator_);
        if (!chip) {
            return Result<SharedPtr<Element>>::Failure(chip.Error());
        }

        ToolTipStyle chipStyle;
        chipStyle.background = style.shortcutBackground;
        chipStyle.padding = style.shortcutPadding;
        chipStyle.text = style.shortcut;
        status = chip.Value()->Construct({.text = args.shortcut, .style = &chipStyle});
        if (status) {
            status = header.Value()->TryAddSlot(chip.Value(),
                                                {.before = args.title ? style.shortcutGap : 0.0f,
                                                 .alignment = StackAlignment::Center});
        }

        shortcut_ = Move(chip.Value());
    }

    if (status && (args.title || args.shortcut)) {
        status = column.Value()->TryAddSlot(header.Value(), {});
    }

    if (status && args.description) {
        status = column.Value()->TryAddSlot(
            args.description, {.before = (args.title || args.shortcut) ? style.lineGap : 0.0f});
    }

    if (!status) {
        return Result<SharedPtr<Element>>::Failure(status.Error());
    }

    return Result<SharedPtr<Element>>::Success(column.Value());
}

Result<void> ToolTip::SetStyle(const ToolTipStyle& style) noexcept {
    if (!IsValidStyle(style)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto padded = SetPadding(style.padding);
    if (!padded) {
        return padded;
    }

    style_ = style;
    InvalidatePaint();
    return ApplyTextStyle();
}

const ToolTipStyle& ToolTip::GetStyle() const noexcept {
    return style_;
}

Result<void> ToolTip::ApplyTextStyle() noexcept {
    // 글자·설명·제목에 색과 줄바꿈 폭을 적용한다.
    const auto apply = [this](TextLabel* label, const TextStyle& text,
                              bool wraps) noexcept -> Result<void> {
        if (!label) {
            return Result<void>::Success();
        }

        label->SetColor(text.color);
        // 줄바꿈 폭이 0이면(단축키 표지) 한 줄로 둔다.
        return wraps ? label->SetLayoutOptions(style_.wrapWidth, text::TextAlignment::Left,
                                               style_.wrapWidth > 0)
                     : Result<void>::Success();
    };
    auto applied = apply(text_.Get(), style_.text, true);
    if (applied) {
        applied = apply(description_.Get(), style_.description, true);
    }

    if (applied) {
        applied = apply(title_.Get(), style_.title, true);
    }

    if (!applied) {
        return applied;
    }

    if (shortcut_) {
        ToolTipStyle chipStyle;
        chipStyle.background = style_.shortcutBackground;
        chipStyle.padding = style_.shortcutPadding;
        chipStyle.text = style_.shortcut;
        return shortcut_->SetStyle(chipStyle);
    }

    return Result<void>::Success();
}

ColorRef ToolTip::GetForegroundColor() const noexcept {
    return style_.text.color;
}

Result<void> ToolTip::OnPaint(PaintList& list, int32 layer) const noexcept {
    if (style_.background.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(GetGeometry(),
                            style_.background.Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

} // namespace rocks::ui
