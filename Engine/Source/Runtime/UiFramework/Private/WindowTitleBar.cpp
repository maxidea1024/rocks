#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/ToolTip.h>
#include <UiFramework/WindowTitleBar.h>

namespace rocks::ui {

namespace {

constexpr usize ButtonCount = 3;
// 창 버튼 툴팁 글자다(UE SWindowTitleBar.cpp 636–653행).
constexpr StringView MinimizeToolTip = "Minimize";
constexpr StringView MaximizeToolTip = "Maximize";
constexpr StringView RestoreToolTip = "Restore Down";
constexpr StringView CloseToolTip = "Close";
constexpr StringView ExitFullscreenToolTip = "Exit Fullscreen";
constexpr WindowButtonRole ButtonOrder[ButtonCount] = {
    WindowButtonRole::Minimize, WindowButtonRole::Maximize, WindowButtonRole::Close};

} // namespace

WindowTitleBar::WindowTitleBar(Allocator& allocator, const WindowStyle& style) noexcept
    : Element(allocator), allocator_(allocator), style_(style) {
}

Result<void> WindowTitleBar::Construct(Args&& args) noexcept {
    Allocator& allocator = allocator_;
    if (args.showAppIcon) {
        auto icon = SharedPtr<WindowAppIcon>::TryMake(allocator, allocator, style_);
        if (!icon) {
            return Result<void>::Failure(icon.Error());
        }

        icon_ = Move(icon.Value());
        auto added = TryAddChild(icon_);
        if (!added) {
            return added;
        }
    }

    if (args.leftContent) {
        left_ = Move(args.leftContent);
        auto added = TryAddChild(left_);
        if (!added) {
            return added;
        }
    }

    makeLabel_ = Move(args.makeLabel);
    if (args.title) {
        auto titled = SetTitle(Move(args.title));
        if (!titled) {
            return titled;
        }
    }

    Delegate<void()>* actions[ButtonCount] = {&args.onMinimize, &args.onToggleMaximize,
                                              &args.onClose};
    for (usize i = 0; i < ButtonCount; ++i) {
        auto button =
            SharedPtr<WindowControlButton>::TryMake(allocator, allocator, ButtonOrder[i], style_);
        if (!button) {
            return Result<void>::Failure(button.Error());
        }

        button.Value()->SetOnClicked(Move(*actions[i]));
        // 창이 허용하지 않는 동작의 버튼은 끈다(UE IsEnabled = HasMinimizeBox·HasMaximizeBox).
        const bool allowed = (ButtonOrder[i] != WindowButtonRole::Minimize || args.canMinimize) &&
                             (ButtonOrder[i] != WindowButtonRole::Maximize || args.canMaximize);
        button.Value()->SetEnabled(allowed);
        buttons_[i] = Move(button.Value());
        auto added = TryAddChild(buttons_[i]);
        if (added && makeLabel_.IsBound()) {
            added = AttachToolTip(*buttons_[i], ButtonOrder[i]);
        }

        if (!added) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> WindowTitleBar::AttachToolTip(WindowControlButton& button,
                                           WindowButtonRole role) noexcept {
    // 최대화 버튼의 글자는 열 때의 상태(최대화 중이면 "Restore Down")를 따른다.
    return button.SetToolTipFactory(Element::ToolTipFactory::CreateLambda(
        [this, &button, role]() noexcept -> Result<SharedPtr<Element>> {
            const StringView text = role == WindowButtonRole::Minimize ? MinimizeToolTip
                                    : role == WindowButtonRole::Close  ? CloseToolTip
                                    : button.IsFullscreen()            ? ExitFullscreenToolTip
                                    : button.IsMaximized()             ? RestoreToolTip
                                                                       : MaximizeToolTip;
            auto label = makeLabel_.Execute(text, DefaultToolTipStyle().text.font);
            if (!label) {
                return Result<SharedPtr<Element>>::Failure(label.Error());
            }

            auto tip = SharedPtr<ToolTip>::TryMake(allocator_, allocator_);
            if (!tip) {
                return Result<SharedPtr<Element>>::Failure(tip.Error());
            }

            auto built = tip.Value()->Construct({.text = label.Value()});
            return built ? Result<SharedPtr<Element>>::Success(tip.Value())
                         : Result<SharedPtr<Element>>::Failure(built.Error());
        }));
}

Result<void> WindowTitleBar::SetTitle(SharedPtr<TextLabel> title) noexcept {
    if (title_) {
        (void)RemoveChild(*title_);
    }

    title_ = Move(title);
    if (!title_) {
        InvalidateLayout();
        return Result<void>::Success();
    }

    title_->SetColor(style_.title.color);
    // 좁은 창에서 버튼·왼쪽 내용 밑으로 그리지 않게 자기 칸에서 자른다.
    title_->SetClipToBounds(true);
    (void)title_->SetRenderOpacity(active_ ? 1.0f : InactiveWindowTitleOpacity);
    return TryAddChild(title_);
}

void WindowTitleBar::SetActive(bool active) noexcept {
    if (active_ == active) {
        return;
    }

    active_ = active;
    const float opacity = active ? 1.0f : InactiveWindowTitleOpacity;
    if (title_) {
        (void)title_->SetRenderOpacity(opacity);
    }

    if (icon_) {
        (void)icon_->SetRenderOpacity(opacity);
    }
}

bool WindowTitleBar::IsActive() const noexcept {
    return active_;
}

float WindowTitleBar::DesiredHeight(const WindowStyle& style, float titleHeight) noexcept {
    const float icon = style.appIconSize.y + style.appIconPadding.y + style.appIconPadding.w;
    const float title = titleHeight + style.titlePadding.y + style.titlePadding.w;
    return math::Max(math::Max(style.buttonSize.y, icon), title);
}

void WindowTitleBar::SetFullscreen(bool fullscreen) noexcept {
    if (const auto& button = ControlButton(WindowButtonRole::Maximize)) {
        button->SetFullscreen(fullscreen);
    }
}

void WindowTitleBar::SetMaximized(bool maximized) noexcept {
    if (const auto& button = ControlButton(WindowButtonRole::Maximize)) {
        button->SetMaximized(maximized);
    }
}

const SharedPtr<WindowControlButton>&
WindowTitleBar::ControlButton(WindowButtonRole role) const noexcept {
    return buttons_[static_cast<usize>(role)];
}

input::WindowRegion WindowTitleBar::GetWindowRegion() const noexcept {
    return input::WindowRegion::Caption;
}

Vec2 WindowTitleBar::ComputeDesiredSize(float) const noexcept {
    // 높이는 버튼·아이콘·제목 중 큰 것, 폭은 모두를 늘어놓은 합이다.
    const Vec2 iconSize = icon_ ? icon_->GetDesiredSize() : Vec2{};
    const Vec2 leftSize = left_ ? left_->GetDesiredSize() : Vec2{};
    const Vec4 pad = style_.titlePadding;
    const Vec2 titleSize =
        title_ ? title_->GetDesiredSize() + Vec2{pad.x + pad.z, pad.y + pad.w} : Vec2{};
    const float buttons = style_.buttonSize.x * static_cast<float>(ButtonCount);
    const float height =
        math::Max(math::Max(style_.buttonSize.y, iconSize.y), math::Max(leftSize.y, titleSize.y));
    return {iconSize.x + leftSize.x + titleSize.x + buttons, height};
}

Result<void> WindowTitleBar::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    const Vec2 size = geometry.size;
    const auto place = [this](Element& child, Vec2 at, Vec2 extent) noexcept {
        return child.ArrangeIfNeeded(
            GetGeometry().MakeChild(extent, {1, 0, 0, 1, at.x, at.y}, {}, {}));
    };

    // 버튼은 오른쪽 위에 닫기·최대화·최소화 순으로 붙인다(UE VAlign_Top).
    float buttonsLeft = size.x;
    for (usize i = ButtonCount; i > 0; --i) {
        const auto& button = buttons_[i - 1];
        if (!button) {
            continue;
        }

        buttonsLeft -= style_.buttonSize.x;
        auto placed = place(*button, {buttonsLeft, 0}, style_.buttonSize);
        if (!placed) {
            return placed;
        }
    }

    float leftEnd = 0;
    if (icon_) {
        const Vec2 iconSize = icon_->GetDesiredSize();
        auto placed = place(*icon_, {0, 0}, {iconSize.x, math::Min(size.y, iconSize.y)});
        if (!placed) {
            return placed;
        }

        leftEnd = iconSize.x;
    }

    if (left_) {
        const Vec2 leftSize = left_->GetDesiredSize();
        const float width = math::Max(0.0f, math::Min(leftSize.x, buttonsLeft - leftEnd));
        auto placed = place(*left_, {leftEnd, 0}, {width, size.y});
        if (!placed) {
            return placed;
        }

        leftEnd += width;
    }

    if (title_) {
        // 제목은 막대 전체의 가운데에 두되 왼쪽 내용과 버튼 사이로 줄인다(UE 제목 overlay 가운데).
        const Vec4 pad = style_.titlePadding;
        const Vec2 desired = title_->GetDesiredSize();
        const float available = math::Max(0.0f, buttonsLeft - leftEnd - pad.x - pad.z);
        const float width = math::Min(desired.x, available);
        float x = (size.x - width) * 0.5f;
        x = math::Clamp(x, leftEnd + pad.x,
                        math::Max(leftEnd + pad.x, buttonsLeft - pad.z - width));
        const float height = math::Min(desired.y, size.y);
        auto placed = place(*title_, {x, (size.y - height) * 0.5f}, {width, height});
        if (!placed) {
            return placed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
