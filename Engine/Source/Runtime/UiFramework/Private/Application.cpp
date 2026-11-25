#include <UiFramework/Application.h>
#include <Platform/Clock.h>

namespace rocks::ui {

Application::Application(Allocator& allocator) noexcept
    : allocator_(allocator), cache_(allocator), hitGrid_(allocator), timers_(allocator),
      toolTips_(allocator), hover_(allocator), captures_(allocator), focus_(allocator),
      modifiers_(allocator), commandScopes_(allocator) {
    cache_.SetTheme(&theme_);
}

void Application::SetTheme(const ThemeColors& theme) noexcept {
    if (theme == theme_) {
        return;
    }

    theme_ = theme;
    ++themeRevision_;
    cache_.SetTheme(&theme_, themeRevision_);
}

const ThemeColors& Application::Theme() const noexcept {
    return theme_;
}

Application::~Application() noexcept {
    if (context_) {
        *context_ = nullptr;
        context_.Reset();
    }

    ResetInput();
}

void Application::SetRoot(const SharedPtr<Element>& root) noexcept {
    ResetInput();
    root_ = root;
    cache_.Reset();
    hitGrid_.Reset();
}

void Application::ResetInput() noexcept {
    while (!toolTips_.IsEmpty()) {
        CloseToolTip(toolTips_[toolTips_.Size() - 1].user);
    }

    modifiers_.Clear();
    CancelAllPointers();
    while (!hover_.IsEmpty()) {
        ClearHover(hover_[hover_.Size() - 1].user);
    }

    ClearAllFocus();
}

input::WindowRegion Application::QueryWindowRegion(Vec2 position) const noexcept {
    if (!root_) {
        return input::WindowRegion::Client;
    }

    const bool snapshot = hitGrid_.HasSnapshot();
    const auto hit = snapshot ? hitGrid_.Find(position, *root_) : SharedPtr<Element>{};
    auto* target = snapshot ? hit.Get() : root_->HitTest(position);
    while (target) {
        const auto region = target->GetWindowRegion();
        if (region != input::WindowRegion::Client || target->SupportsKeyboardFocus()) {
            return region;
        }

        target = target->Parent();
    }

    return input::WindowRegion::Client;
}

input::MouseCursor Application::QueryMouseCursor(Vec2 position, uint32 userId) const noexcept {
    if (!root_) {
        return input::MouseCursor::Arrow;
    }

    for (const auto& capture : captures_) {
        if (capture.source == PointerSource::Mouse && capture.userId == userId &&
            capture.pointerId == 0) {
            auto target = capture.target.Lock();
            if (target && target->IsInputPathActive(*root_, true)) {
                return target->ResolveMouseCursor(position);
            }
        }
    }

    if (hitGrid_.HasSnapshot()) {
        auto target = hitGrid_.Find(position, *root_);
        return target ? target->ResolveMouseCursor(position) : input::MouseCursor::Arrow;
    }

    auto* target = root_->HitTest(position);
    return target ? target->ResolveMouseCursor(position) : input::MouseCursor::Arrow;
}

Result<void> Application::BuildPaintList(const Geometry& geometry, float layoutScale,
                                         PaintList& list) noexcept {
    auto prepared = PrepareLayout(geometry, layoutScale);
    if (!prepared) {
        return prepared;
    }

    list.Clear();
    list.SetTheme(&theme_, themeRevision_);
    auto painted = root_->Paint(list, 0);
    return painted ? hitGrid_.Update(root_) : Move(painted);
}

Result<const CacheRoot*> Application::UpdatePaintCache(const Geometry& geometry,
                                                       float layoutScale) noexcept {
    auto prepared = PrepareLayout(geometry, layoutScale);
    if (!prepared) {
        return Result<const CacheRoot*>::Failure(prepared.Error());
    }

    return UpdateArrangedPaintCache();
}

Result<const CacheRoot*> Application::UpdateArrangedPaintCache() noexcept {
    if (!root_) {
        return Result<const CacheRoot*>::Failure(ErrorCode::NotReady);
    }

    auto updated = cache_.Update(*root_, 0);
    if (updated) {
        updated = hitGrid_.Update(root_);
    }

    if (!updated) {
        return Result<const CacheRoot*>::Failure(updated.Error());
    }

    return Result<const CacheRoot*>::Success(&cache_);
}

Result<void> Application::PrepareLayout(const Geometry& geometry, float layoutScale) noexcept {
    AdvanceTimers();
    AdvanceToolTips();
    ReleaseInactiveTargets();
    SharedPtr<Element> root = root_;
    if (!root) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    root->UpdateBoundAttributes();
    auto measured = root->Prepass(layoutScale);
    if (!measured) {
        return measured;
    }

    return root->ArrangeIfNeeded(geometry);
}

bool Application::HasAnyUserFocus(const Element& element) const noexcept {
    for (const auto& entry : focus_) {
        if (entry.target.Lock().Get() == &element && root_ &&
            element.IsInputPathActive(*root_, false)) {
            return true;
        }
    }

    return false;
}

bool Application::HasPointerCapture(PointerSource source, uint32 user,
                                    uint32 pointer) const noexcept {
    for (const auto& capture : captures_) {
        if (capture.userId == user && capture.pointerId == pointer && capture.source == source) {
            auto target = capture.target.Lock();
            return target && root_ && target->IsInputPathActive(*root_, true);
        }
    }

    return false;
}

bool Application::HasMouseCapture(const Element& element, uint32 user,
                                  uint32 pointer) const noexcept {
    for (const auto& capture : captures_) {
        if (capture.userId == user && capture.pointerId == pointer &&
            capture.source == PointerSource::Mouse && capture.target.Lock().Get() == &element &&
            root_ && element.IsInputPathActive(*root_, true)) {
            return true;
        }
    }

    return false;
}

SharedPtr<Element> Application::GetFocus(uint32 userId) const noexcept {
    for (const auto& focus : focus_) {
        if (focus.userId == userId) {
            return focus.target.Lock();
        }
    }

    return {};
}

Result<void> Application::SetFocus(uint32 userId, Element& target) noexcept {
    auto owned = root_.Get() == &target ? root_
                                        : (target.Parent() ? target.Parent()->FindChild(&target)
                                                           : SharedPtr<Element>{});
    return owned ? SetFocus(userId, owned) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> Application::SetFocus(uint32 userId, const SharedPtr<Element>& target) noexcept {
    if (focusChanging_ || cancelling_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (target && (!target->SupportsKeyboardFocus() || !IsFocusablePath(target.Get()))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    usize index = focus_.Size();
    for (usize i = 0; i < focus_.Size(); ++i) {
        if (focus_[i].userId == userId) {
            index = i;
            break;
        }
    }

    SharedPtr<Element> previous =
        index < focus_.Size() ? focus_[index].target.Lock() : SharedPtr<Element>();
    if (previous.Get() == target.Get() && (target || index == focus_.Size())) {
        return Result<void>::Success();
    }

    if (target && index == focus_.Size()) {
        auto added = focus_.TryPushBack({userId, WeakPtr<Element>(target)});
        if (!added) {
            return added;
        }
    } else if (target) {
        focus_[index].target = WeakPtr<Element>(target);
    } else if (index < focus_.Size()) {
        (void)focus_.TryRemoveAt(index);
    }

    ++focusVersion_;
    focusChanging_ = true;
    if (previous) {
        previous->OnFocusLost(userId);
    }

    if (target && IsFocusablePath(target.Get())) {
        target->OnFocusReceived(userId);
    }

    focusChanging_ = false;
    return Result<void>::Success();
}

bool Application::RouteGamepadButtons(uint32 user, uint32 pressed,
                                      const SharedPtr<Element>& scope) noexcept {
    auto target = GetFocus(user);
    if (!target || !IsFocusablePath(target.Get()) || (scope && !target->IsWithin(*scope))) {
        return false;
    }

    for (uint32 i = 0; i < input::GamepadButtonCount; ++i) {
        if ((pressed & (uint32{1} << i)) &&
            target->OnGamepadButtonDown(user, static_cast<input::GamepadButton>(i))) {
            return true;
        }
    }

    return false;
}

Result<bool> Application::RouteKey(uint32 userId, const input::InputEvent& event) noexcept {
    return RouteKeyWithin(userId, event, {});
}

Result<bool> Application::RouteKeyWithin(uint32 userId, const input::InputEvent& event,
                                         const SharedPtr<Element>& scope) noexcept {
    if (event.type != input::InputEventType::KeyDown &&
        event.type != input::InputEventType::KeyUp && event.type != input::InputEventType::Text &&
        event.type != input::InputEventType::TextComposition &&
        event.type != input::InputEventType::TextCompositionCommit) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    if (event.type == input::InputEventType::KeyDown) {
        CloseToolTip(userId);
    }

    auto chord = UpdateChord(userId, event);
    if (!chord) {
        return Result<bool>::Failure(chord.Error());
    }

    const bool down = event.type == input::InputEventType::KeyDown;
    SharedPtr<Element> target;
    for (usize i = 0; i < focus_.Size(); ++i) {
        if (focus_[i].userId == userId) {
            target = focus_[i].target.Lock();
            break;
        }
    }

    if (!target || !IsFocusablePath(target.Get())) {
        auto cleared = SetFocus(userId, {});
        if (!cleared) {
            return Result<bool>::Failure(cleared.Error());
        }

        return Result<bool>::Success(down && !scope &&
                                     ProcessCommands(nullptr, chord.Value(), event.repeat));
    }

    Array<SharedPtr<Element>> path(allocator_);
    if (scope && !target->IsWithin(*scope)) {
        return Result<bool>::Success(false);
    }

    auto pathResult = BuildPath(target.Get(), scope ? scope : root_, path);
    if (!pathResult) {
        return Result<bool>::Failure(pathResult.Error());
    }

    if (event.type == input::InputEventType::KeyDown) {
        for (usize i = path.Size(); i > 0; --i) {
            if (path[i - 1]->OnPreviewKeyChord(userId, event, chord.Value())) {
                return Result<bool>::Success(true);
            }
        }
    }

    for (usize i = 0; i < path.Size(); ++i) {
        if ((down && ProcessCommands(path[i].Get(), chord.Value(), event.repeat)) ||
            path[i]->OnKeyEvent(userId, event)) {
            return Result<bool>::Success(true);
        }
    }

    return Result<bool>::Success(down && !scope &&
                                 ProcessCommands(nullptr, chord.Value(), event.repeat));
}

void Application::CancelAllPointers() noexcept {
    if (cancelling_) {
        return;
    }

    cancelling_ = true;
    while (captures_.Size() != 0) {
        ReleaseCapture(captures_[0].source, captures_[0].userId, captures_[0].pointerId);
    }

    cancelling_ = false;
}

void Application::CancelPointers(PointerSource source) noexcept {
    if (cancelling_) {
        return;
    }

    cancelling_ = true;
    usize index = 0;
    while (index < captures_.Size()) {
        if (captures_[index].source == source) {
            ReleaseCapture(source, captures_[index].userId, captures_[index].pointerId);
        } else {
            ++index;
        }
    }

    cancelling_ = false;
}

Result<bool> Application::RoutePointer(const PointerEvent& event) noexcept {
    return RoutePointerWithin(event, {});
}

Result<bool> Application::RoutePointerWithin(const PointerEvent& event,
                                             const SharedPtr<Element>& scope) noexcept {
    if (cancelling_) {
        return Result<bool>::Failure(ErrorCode::Busy);
    }

    SharedPtr<Element> root = scope ? scope : root_;
    if (!root || (scope && !IsFocusablePath(scope.Get()))) {
        return Result<bool>::Success(false);
    }

    SharedPtr<Element> captured;
    Vec2 pressPosition{};
    bool allowTouchPan = false;
    bool releaseOnUp = event.source == PointerSource::Touch;
    for (usize i = 0; i < captures_.Size(); ++i) {
        if (captures_[i].source == event.source && captures_[i].userId == event.userId &&
            captures_[i].pointerId == event.pointerId) {
            captured = captures_[i].target.Lock();
            pressPosition = captures_[i].pressPosition;
            allowTouchPan = captures_[i].allowTouchPan;
            releaseOnUp = releaseOnUp || captures_[i].button == event.button;
            if (!captured) {
                ReleaseCapture(event.source, event.userId, event.pointerId);
            }
            break;
        }
    }

    // 분리된 Element가 이전 capture를 계속 받지 않도록 현재 루트 소속을 확인한다.
    if (captured && (!IsFocusablePath(captured.Get()) || (scope && !captured->IsWithin(*scope)))) {
        captured.Reset();
        ReleaseCapture(event.source, event.userId, event.pointerId);
    }

    SharedPtr<Element> recorded;
    Element* hit = nullptr;
    if (!captured || event.source == PointerSource::Mouse) {
        if (hitGrid_.HasSnapshot()) {
            recorded = hitGrid_.Find(event.position, *root);
            hit = recorded.Get();
        } else {
            // 아직 첫 프레임을 그리지 않은 호스트에서는 직접 지정한 Geometry를 쓴다.
            hit = root->HitTest(event.position);
        }
    }

    if (event.source == PointerSource::Mouse) {
        const uint64 previousHoverVersion = hoverVersion_;
        auto hovered =
            UpdateHover(event.userId, event.type == PointerEventType::Cancel ? nullptr : hit, root);
        if (!hovered) {
            return Result<bool>::Failure(hovered.Error());
        }

        if (!root_ || (scope ? !IsFocusablePath(scope.Get()) : root != root_)) {
            return Result<bool>::Success(false);
        }

        // 진입/이탈 콜백에서 트리가 바뀔 수 있다. 같은 경로 위의 이동은 한 번만 조회한다.
        if (previousHoverVersion != hoverVersion_) {
            if (hitGrid_.HasSnapshot()) {
                recorded = hitGrid_.Find(event.position, *root);
                hit = recorded.Get();
            } else {
                hit = root->HitTest(event.position);
            }
        }
    }

    if (event.source == PointerSource::Mouse) {
        ObserveToolTip(event, hit, root);
        // 마지막 툴팁을 닫으면 임시 루트가 해제된다. 같은 누름은 원래 화면에 계속 전달한다.
        if (!scope && root != root_) {
            root = root_;
            if (!root) {
                return Result<bool>::Success(false);
            }

            recorded.Reset();
            hit = root->HitTest(event.position);
        }
    }

    if (captured && captured->IsInputPathActive(*root, true)) {
        hit = captured.Get();
    }

    if (hit == nullptr) {
        return Result<bool>::Success(false);
    }

    Array<SharedPtr<Element>> path(allocator_);
    auto pathResult = BuildPath(hit, root, path);
    if (!pathResult) {
        return Result<bool>::Failure(pathResult.Error());
    }

    // 이벤트 전달 후 capture 요청을 저장할 공간을 미리 확보한다.
    auto reserved = captures_.TryEnsureCapacity(captures_.Size() + 1);
    if (!reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    auto applyReply = [&](const PointerReply& reply,
                          const SharedPtr<Element>& target) noexcept -> Result<void> {
        if (reply.releaseCapture) {
            ReleaseCapture(event.source, event.userId, event.pointerId);
        } else if (reply.capture) {
            if (!IsFocusablePath(target.Get())) {
                target->OnPointerCaptureLost(event.source, event.userId, event.pointerId);
                return Result<void>::Success();
            }

            auto stored = SetCapture(event, target, reply.allowTouchPan);
            if (!stored) {
                return stored;
            }
        }

        return Result<void>::Success();
    };

    const uint64 focusBefore = focusVersion_;
    bool handled = false;
    if (captured && allowTouchPan && event.source == PointerSource::Touch &&
        event.type == PointerEventType::Move) {
        for (const auto& candidate : path) {
            const auto reply = candidate->OnTouchPan(event, pressPosition);
            if (!reply.capture) {
                continue;
            }

            auto applied = applyReply(reply, candidate);
            if (!applied) {
                return Result<bool>::Failure(applied.Error());
            }

            auto cancelled = event;
            cancelled.type = PointerEventType::Cancel;
            for (const auto& previous : path) {
                previous->OnPointerReleaseInPath(cancelled);
            }

            handled = true;
            break;
        }
    }

    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
        for (usize i = path.Size(); i > 0; --i) {
            path[i - 1]->OnPointerReleaseInPath(event);
        }
    }

    if (event.type == PointerEventType::Down) {
        for (usize i = captured ? 1 : path.Size(); i > 0; --i) {
            const PointerReply reply = path[i - 1]->OnPreviewPointerDown(event);
            auto applied = applyReply(reply, path[i - 1]);
            if (!applied) {
                return Result<bool>::Failure(applied.Error());
            }

            if (reply.handled) {
                handled = true;
                break;
            }
        }
    }

    if (!handled) {
        for (usize i = 0; i < path.Size(); ++i) {
            const PointerReply reply = path[i]->OnPointerEvent(event);
            auto applied = applyReply(reply, path[i]);
            if (!applied) {
                return Result<bool>::Failure(applied.Error());
            }

            if (reply.handled) {
                handled = true;
                break;
            }

            // capture된 휠은 소유자 경로를 bubble하고, 나머지는 소유자에게 직접 전달한다.
            if (captured && event.type != PointerEventType::Wheel) {
                break;
            }
        }
    }

    // 이벤트 처리 중 focus가 바뀌지 않았다면 경로에서 가장 안쪽의 focus 가능 Element를 고른다.
    if (event.type == PointerEventType::Down && focusVersion_ == focusBefore) {
        for (usize i = 0; i < path.Size(); ++i) {
            if (path[i]->SupportsKeyboardFocus() && IsFocusablePath(path[i].Get())) {
                auto focused = SetFocus(event.userId, path[i]);
                if (!focused) {
                    return Result<bool>::Failure(focused.Error());
                }
                break;
            }
        }
    }

    if ((event.type == PointerEventType::Up && releaseOnUp) ||
        event.type == PointerEventType::Cancel) {
        ReleaseCapture(event.source, event.userId, event.pointerId);
    }

    return Result<bool>::Success(handled);
}

bool Application::IsFocusablePath(const Element* element) const noexcept {
    return root_ && element && element->IsInputPathActive(*root_, false);
}

Result<void> Application::BuildPath(Element* target, const SharedPtr<Element>& root,
                                    Array<SharedPtr<Element>>& path) const noexcept {
    for (Element* current = target; current != nullptr; current = current->Parent()) {
        Element* parent = current->Parent();
        if (current != root.Get() && parent == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        SharedPtr<Element> keep = current == root.Get() ? root : parent->FindChild(current);
        if (!keep) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        auto added = path.TryPushBack(keep);
        if (!added) {
            return added;
        }

        if (current == root.Get()) {
            return Result<void>::Success();
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

void Application::ClearAllFocus() noexcept {
    if (focusChanging_) {
        return;
    }

    focusChanging_ = true;
    ++focusVersion_;
    while (focus_.Size() != 0) {
        const uint32 userId = focus_[0].userId;
        SharedPtr<Element> previous = focus_[0].target.Lock();
        (void)focus_.TryRemoveAt(0);
        if (previous) {
            previous->OnFocusLost(userId);
        }
    }

    focusChanging_ = false;
}

void Application::ReleaseInputForSubtree(const Element& subtree) noexcept {
    ReleaseInactiveTargets(&subtree);
}

void Application::ReleaseInactiveTargets(const Element* releasing) noexcept {
    if (cancelling_ || focusChanging_) {
        return;
    }

    // 페이지 교체나 자식 제거 후 다음 입력이 없어도 눌림 상태를 정리한다.
    cancelling_ = true;
    for (usize i = 0; i < captures_.Size();) {
        const auto target = captures_[i].target.Lock();
        if (!target || (releasing && target->IsWithin(*releasing)) ||
            !IsFocusablePath(target.Get())) {
            ReleaseCapture(captures_[i].source, captures_[i].userId, captures_[i].pointerId);
        } else {
            ++i;
        }
    }

    cancelling_ = false;

    focusChanging_ = true;
    for (usize i = 0; i < focus_.Size();) {
        const auto target = focus_[i].target.Lock();
        if (target && !(releasing && target->IsWithin(*releasing)) &&
            IsFocusablePath(target.Get())) {
            ++i;
            continue;
        }

        const uint32 userId = focus_[i].userId;
        (void)focus_.TryRemoveAt(i);
        ++focusVersion_;
        if (target) {
            target->OnFocusLost(userId);
        }
    }

    focusChanging_ = false;
}

void Application::ReleaseCapture(PointerSource source, uint32 userId, uint32 pointerId) noexcept {
    for (usize i = 0; i < captures_.Size(); ++i) {
        if (captures_[i].source == source && captures_[i].userId == userId &&
            captures_[i].pointerId == pointerId) {
            SharedPtr<Element> target = captures_[i].target.Lock();
            (void)captures_.TryRemoveAt(i);
            if (target) {
                target->OnPointerCaptureLost(source, userId, pointerId);
            }

            return;
        }
    }
}

Result<void> Application::SetCapture(const PointerEvent& event, const SharedPtr<Element>& target,
                                     bool allowTouchPan) noexcept {
    for (usize i = 0; i < captures_.Size(); ++i) {
        if (captures_[i].source == event.source && captures_[i].userId == event.userId &&
            captures_[i].pointerId == event.pointerId &&
            captures_[i].target.Lock().Get() == target.Get()) {
            captures_[i].allowTouchPan = allowTouchPan;
            return Result<void>::Success();
        }
    }

    ReleaseCapture(event.source, event.userId, event.pointerId);
    return captures_.TryPushBack({event.source, event.userId, event.pointerId, event.button,
                                  WeakPtr<Element>(target), event.position, allowTouchPan});
}

Result<bool> Application::MoveFocus(uint32 userId, bool forward, bool wrap) noexcept {
    return MoveFocusWithin(userId, root_, forward, wrap);
}

Result<bool> Application::MoveFocusWithin(uint32 userId, const SharedPtr<Element>& root,
                                          bool forward, bool wrap) noexcept {
    return root ? NavigateInternal(
                      userId, forward ? FocusDirection::Next : FocusDirection::Previous, root, wrap)
                : Result<bool>::Success(false);
}

Result<void> Application::CollectFocusTargets(const SharedPtr<Element>& element,
                                              Array<SharedPtr<Element>>& targets) const noexcept {
    if (!element->IsEnabled() || element->GetVisibility() == Visibility::Collapsed ||
        element->GetVisibility() == Visibility::Hidden) {
        return Result<void>::Success();
    }

    if (element->SupportsKeyboardFocus()) {
        auto added = targets.TryPushBack(element);
        if (!added) {
            return added;
        }
    }

    for (const auto& child : element->ActiveChildren()) {
        auto collected = CollectFocusTargets(child, targets);
        if (!collected) {
            return collected;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
