#include <Base/NumericLimits.h>
#include <UiFramework/Controls/ComboBox.h>
#include <UiFramework/Scrolling/ScrollView.h>
#include <UiFramework/Layout/VerticalStack.h>
#include <Core/ScopeExit.h>

namespace rocks::ui {

namespace {

// 콤보 상자 기본 크기와 펼친 목록 행 높이다(UE 에디터 콤보 최소 크기·행 높이).
constexpr Vec2 ComboDefaultSize{80, 24};
constexpr float ComboRowHeight = 24;

} // namespace

ComboBox::ComboBox(Allocator& allocator) noexcept
    : MenuAnchor(allocator), allocator_(allocator), rows_(allocator) {
    SetDesiredSize(ComboDefaultSize);
    // 내용은 세로 가운데다(UE SComboButton 기본 VAlign_Center — SComboButton.h 45행).
    SetContentAlignment(StackAlignment::Fill, StackAlignment::Center);
    SetPlacement(MenuPlacement::ComboBox);
    SetMenuFactory(MenuFactory::CreateRaw(this, &ComboBox::MakeMenu));
    // 화살표 칸(여백 2 + 16 + 여백 2)을 내용 오른쪽에 비워 둔다.
    (void)SetContentPadding({0, 0, ComboArrowPadding * 2 + ComboArrowSize, 0});
}

Result<void> ComboBox::OnPaint(PaintList& list, int32 layer) const noexcept {
    auto painted = MenuAnchor::OnPaint(list, layer);
    if (!painted) {
        return painted;
    }

    const auto& geometry = GetGeometry();
    const float right = GetStyle().normalPadding.z;
    const float x = geometry.size.x - right - ComboArrowPadding - ComboArrowSize;
    const float y = (geometry.size.y - ComboArrowSize) * 0.5f;
    if (x < 0 || geometry.size.y <= 0) {
        return Result<void>::Success();
    }

    const bool disabled = !IsEnabled() || list.IsDisabled();
    const StyleColorContext context = list.ColorContext();
    const LinearColor ink = PaintForegroundColor(disabled).Resolve(list.Theme(), context);
    const auto arrow =
        geometry.MakeChild({ComboArrowSize, ComboArrowSize}, {1, 0, 0, 1, x, y}, {}, {});
    return list.TryAddBrush(arrow, Brush::Vector(icons::WideChevronDown, icons::Icon16), ink,
                            layer);
}

Result<void> ComboBox::Construct(Args&& args) noexcept {
    auto limit = SetMaxListHeight(args.maxListHeight);
    if (!limit) {
        return limit;
    }

    SetOptionFactory(Move(args.makeOption));
    SetOnSelectionChanged(Move(args.onSelectionChanged));
    SetOnOpening(Move(args.onOpening));
    return Refresh(args.count, args.selected);
}

void ComboBox::DisconnectModel() noexcept {
    DisconnectMenu();
    makeOption_.Unbind();
    selectionChanged_.Unbind();
    UpdateOptions(0, -1);
}

Result<void> ComboBox::SetMaxListHeight(float height) noexcept {
    if (!math::IsFinite(height) || height <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    maxListHeight_ = height;
    if (auto bounds = menuBounds_.Lock()) {
        return bounds->SetSizeRange({}, {NumericLimits<float>::Max(), height});
    }

    return Result<void>::Success();
}

void ComboBox::SetOptionFactory(MakeOption&& factory) noexcept {
    makeOption_ = Move(factory);
}

void ComboBox::SetOnSelectionChanged(Delegate<void(int32)>&& callback) noexcept {
    selectionChanged_ = Move(callback);
}

int32 ComboBox::SelectedIndex() const noexcept {
    return selected_;
}

Result<void> ComboBox::SetDisplay(int32 index) noexcept {
    auto old = ChildRefAt(0);
    SharedPtr<Element> display;
    if (index >= 0) {
        if (!makeOption_) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }

        auto made = makeOption_.Execute(static_cast<usize>(index), true);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        display = Move(made.Value());
        if (!display || display->Parent() || IsWithin(*display)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        display->SetVisibility(Visibility::HitTestInvisible);
    }

    if (old) {
        (void)RemoveChild(*old);
    }

    if (display) {
        auto added = TryAddChild(display);
        if (!added) {
            if (old) {
                (void)TryAddChild(old);
            }

            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> ComboBox::Refresh(usize count, int32 selected) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (count > static_cast<usize>(NumericLimits<int32>::Max()) || selected < -1 ||
        (selected >= 0 && static_cast<usize>(selected) >= count)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto updated = SetDisplay(selected);
    if (updated) {
        Close();
        rows_.Clear();
        count_ = count;
        selected_ = selected;
    }

    return updated;
}

void ComboBox::UpdateOptions(usize count, int32 selected) noexcept {
    Close();
    rows_.Clear();
    count_ = count;
    selected_ = selected;
    if (selected < 0) {
        if (auto display = ChildRefAt(0)) {
            (void)RemoveChild(*display);
        }
    }
}

Result<void> ComboBox::Select(int32 index) noexcept {
    if (index == selected_) {
        Close();
        return Result<void>::Success();
    }

    auto changed = Refresh(count_, index);
    if (changed) {
        selectionChanged_.ExecuteIfBound(index);
    }

    return changed;
}

void ComboBox::Highlight(int32 index) noexcept {
    highlighted_ = index;
    if (auto scroll = scroll_.Lock();
        scroll && index >= 0 && static_cast<usize>(index) < rows_.Size()) {
        auto row = rows_[static_cast<usize>(index)].Lock();
        const auto& viewport = scroll->GetGeometry();
        if (row && viewport.size.y > 0) {
            AffineTransform2D inverse;
            if (viewport.render.TryInverse(inverse)) {
                const auto position = inverse.Compose(row->GetGeometry().render).Apply({});
                const float overflow = position.y + row->GetGeometry().size.y - viewport.size.y;
                if (position.y < 0 || overflow > 0) {
                    (void)scroll->SetScrollOffset(scroll->GetScrollOffset() +
                                                  (position.y < 0 ? position.y : overflow));
                }
            }
        }
    }

    for (usize i = 0; i < rows_.Size(); ++i) {
        if (auto row = rows_[i].Lock()) {
            row->SetColor(static_cast<int32>(i) == index ? LinearColor{0.12f, 0.31f, 0.6f, 1}
                                                         : LinearColor{0.11f, 0.12f, 0.14f, 1});
        }
    }
}

Result<SharedPtr<Element>> ComboBox::MakeMenu() noexcept {
    if (!makeOption_ || !Parent() || !count_ || changing_) {
        return Result<SharedPtr<Element>>::Failure(changing_ ? ErrorCode::Busy
                                                             : ErrorCode::NotReady);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto stack = SharedPtr<VerticalStack>::TryMake(allocator_, allocator_);
    if (!stack) {
        return Result<SharedPtr<Element>>::Failure(stack.Error());
    }

    Array<WeakPtr<Button>> rows(allocator_);
    const auto self = Parent()->FindChild(this);
    for (usize i = 0; i < count_; ++i) {
        auto content = makeOption_.Execute(i, false);
        if (!content || !content.Value() || content.Value()->Parent()) {
            return Result<SharedPtr<Element>>::Failure(content ? ErrorCode::InvalidArgument
                                                               : content.Error());
        }

        auto row = SharedPtr<Button>::TryMake(allocator_, allocator_);
        if (!row) {
            return Result<SharedPtr<Element>>::Failure(row.Error());
        }

        content.Value()->SetVisibility(Visibility::HitTestInvisible);
        row.Value()->SetDesiredSize({0, ComboRowHeight});
        auto added = row.Value()->TryAddChild(content.Value());
        if (added) {
            added = stack.Value()->TryAddChild(row.Value());
        }

        if (added) {
            added = rows.TryPushBack(WeakPtr<Button>(row.Value()));
        }

        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }

        row.Value()->SetOnClicked(Delegate<void()>::CreateWeakLambda(self, [this, i]() noexcept {
            if (IsOpen()) {
                (void)Select(static_cast<int32>(i));
            }
        }));
    }

    auto scroll = SharedPtr<ScrollView>::TryMake(allocator_, allocator_);
    if (!scroll) {
        return Result<SharedPtr<Element>>::Failure(scroll.Error());
    }

    auto attached = scroll.Value()->TryAddChild(stack.Value());
    if (!attached) {
        return Result<SharedPtr<Element>>::Failure(attached.Error());
    }

    auto bounds = SharedPtr<SizeBox>::TryMake(allocator_, allocator_);
    if (!bounds) {
        return Result<SharedPtr<Element>>::Failure(bounds.Error());
    }

    attached = bounds.Value()->SetSizeRange({}, {NumericLimits<float>::Max(), maxListHeight_});
    if (attached) {
        attached = bounds.Value()->TryAddChild(scroll.Value());
    }

    if (!attached) {
        return Result<SharedPtr<Element>>::Failure(attached.Error());
    }

    if (selected_ > 0) {
        (void)scroll.Value()->SetScrollOffset(static_cast<float>(selected_) * ComboRowHeight);
    }

    menuBounds_ = WeakPtr<SizeBox>(bounds.Value());
    scroll_ = WeakPtr<ScrollView>(scroll.Value());
    rows_ = Move(rows);
    Highlight(selected_ >= 0 ? selected_ : 0);
    return Result<SharedPtr<Element>>::Success(Move(bounds.Value()));
}

KeyReply ComboBox::OnKeyEvent(const KeyEvent& key) noexcept {
    const uint32 user = key.user;
    const input::InputEvent& event = key.input;
    if (event.type == input::InputEventType::KeyDown && count_) {
        const bool up = event.key == input::Key::Up;
        if (up || event.key == input::Key::Down) {
            const int32 current = IsOpen() ? highlighted_ : selected_;
            const int32 last = static_cast<int32>(count_) - 1;
            const int32 next =
                up ? (current > 0 ? current - 1 : 0) : (current < last ? current + 1 : last);
            if (IsOpen()) {
                Highlight(next);
            } else {
                (void)Select(next);
            }

            return KeyReply::Handled();
        }

        if (IsOpen() && event.key == input::Key::Enter) {
            (void)Select(highlighted_);
            return KeyReply::Handled();
        }
    }

    return MenuAnchor::OnKeyEvent(key);
}

} // namespace rocks::ui
