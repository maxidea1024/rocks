#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Window/WindowChrome.h>

namespace rocks::ui {

WindowChrome::WindowChrome(Allocator& allocator, const WindowStyle& style) noexcept
    : Overlay(allocator), style_(style) {
}

Result<void> WindowChrome::Construct(Args&& args) noexcept {
    if (!args.titleBar || !args.content) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    titleBar_ = Move(args.titleBar);
    content_ = Move(args.content);
    auto added = TryAddChild(titleBar_);
    if (!added) {
        return added;
    }

    return TryAddChild(content_);
}

void WindowChrome::SetMaximized(bool maximized) noexcept {
    if (maximized_ != maximized) {
        maximized_ = maximized;
        InvalidateLayout();
        InvalidatePaint();
    }

    if (titleBar_) {
        titleBar_->SetMaximized(maximized);
    }
}

void WindowChrome::SetFullscreen(bool fullscreen) noexcept {
    if (fullscreen_ != fullscreen) {
        fullscreen_ = fullscreen;
        InvalidateLayout();
        InvalidatePaint();
    }

    if (titleBar_) {
        titleBar_->SetFullscreen(fullscreen);
    }
}

void WindowChrome::SetActive(bool active) noexcept {
    if (titleBar_) {
        titleBar_->SetActive(active);
    }
}

bool WindowChrome::IsMaximized() const noexcept {
    return maximized_;
}

const SharedPtr<WindowTitleBar>& WindowChrome::TitleBar() const noexcept {
    return titleBar_;
}

const SharedPtr<Element>& WindowChrome::Content() const noexcept {
    return content_;
}

Vec4 WindowChrome::Inset() const noexcept {
    return maximized_ || fullscreen_ ? Vec4{} : style_.borderPadding;
}

Vec2 WindowChrome::ComputeDesiredSize(float) const noexcept {
    const Vec4 inset = Inset();
    const Vec2 bar = titleBar_ ? titleBar_->GetDesiredSize() : Vec2{};
    const Vec4 pad = style_.contentPadding;
    const Vec2 body =
        content_ ? content_->GetDesiredSize() + Vec2{pad.x + pad.z, pad.y + pad.w} : Vec2{};
    return {math::Max(bar.x, body.x) + inset.x + inset.z, bar.y + body.y + inset.y + inset.w};
}

Result<void> WindowChrome::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    const Vec4 inset = Inset();
    const float width = math::Max(0.0f, geometry.size.x - inset.x - inset.z);
    const float height = math::Max(0.0f, geometry.size.y - inset.y - inset.w);
    const float barHeight = titleBar_ ? math::Min(height, titleBar_->GetDesiredSize().y) : 0.0f;
    if (titleBar_) {
        auto placed = titleBar_->ArrangeIfNeeded(
            GetGeometry().MakeChild({width, barHeight}, {1, 0, 0, 1, inset.x, inset.y}, {}, {}));
        if (!placed) {
            return placed;
        }
    }

    if (content_) {
        // 내용은 제목 막대 아래에서 내용 여백만큼 띄운다.
        const Vec4 pad = style_.contentPadding;
        const Vec2 body{math::Max(0.0f, width - pad.x - pad.z),
                        math::Max(0.0f, height - barHeight - pad.y - pad.w)};
        auto placed = content_->ArrangeIfNeeded(GetGeometry().MakeChild(
            body, {1, 0, 0, 1, inset.x + pad.x, inset.y + barHeight + pad.y}, {}, {}));
        if (!placed) {
            return placed;
        }
    }

    // 나머지 자식은 팝업 층이다 — 창 전체를 덮는다(층이 메뉴를 anchor 옆에 놓는다).
    for (usize i = 0; i < ChildCount(); ++i) {
        Element* child = ChildAt(i);
        if (child == titleBar_.Get() || child == content_.Get() ||
            child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        // 팝업이 모니터 작업 영역을 넘지 않게 한다(UW7d — Overlay::Arrange와 같다, 19.5절).
        if (IsHostedPopupLayer(*child)) {
            child->SetPopupArea(PopupLayerArea(*child));
        }

        auto placed = child->ArrangeIfNeeded(GetGeometry().MakeChild(geometry.size, {}, {}, {}));
        if (!placed) {
            return placed;
        }
    }

    return Result<void>::Success();
}

Result<void> WindowChrome::OnPaint(PaintList& list, int32 layer) const noexcept {
    // 바탕 → 틀 → 2 안쪽 외곽선 순이다(최대화 중에는 바탕만).
    const auto draw = [&](const StyleBrush& brush, const Geometry& at) noexcept {
        if (brush.mode == BrushMode::None) {
            return Result<void>::Success();
        }

        return list.TryAddBrush(at, brush.Resolve(list.Theme(), list.ColorContext()),
                                LinearColor{1, 1, 1, 1}, layer);
    };
    const Geometry& whole = GetGeometry();
    auto painted = draw(style_.background, whole);
    if (!painted || maximized_ || fullscreen_) {
        return painted;
    }

    painted = draw(style_.border, whole);
    if (!painted) {
        return painted;
    }

    const float inset = style_.outlineInset;
    const Vec2 inner{math::Max(0.0f, whole.size.x - 2 * inset),
                     math::Max(0.0f, whole.size.y - 2 * inset)};
    return draw(style_.outline, whole.MakeChild(inner, {1, 0, 0, 1, inset, inset}, {}, {}));
}

} // namespace rocks::ui
