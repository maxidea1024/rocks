#include <UiFramework/Application/Application.h>
#include <Core/Diagnostics/Log.h>
#include <Platform/Clock.h>
#include <UiCore/TimeUnits.h>

namespace rocks::ui {

namespace {

constexpr LogCategory LogUiApplication{"UiFramework"};

bool IsModifierKey(input::Key key) noexcept {
    return key == input::Key::LeftShift || key == input::Key::RightShift ||
           key == input::Key::LeftControl || key == input::Key::RightControl ||
           key == input::Key::LeftAlt || key == input::Key::RightAlt ||
           key == input::Key::LeftSuper || key == input::Key::RightSuper;
}

} // namespace

Application::Application(Allocator& allocator) noexcept
    : allocator_(allocator), cache_(allocator), hitGrid_(allocator), timers_(allocator),
      toolTips_(allocator), hover_(allocator), captures_(allocator), focus_(allocator),
      modifiers_(allocator), commandScopes_(allocator), pressObservers_(allocator),
      windowCommands_(allocator), navigationConfigs_(allocator) {
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

void Application::SetVectorImageResolver(VectorImageResolver* resolver) noexcept {
    if (vectorImages_ == resolver) {
        return;
    }

    vectorImages_ = resolver;
    vectorRevision_ = resolver ? resolver->Revision() : 0;
    cache_.SetVectorImageResolver(resolver);
    // 지역 캐시(CachePanel)는 테마 revision이 바뀌어야 다시 기록한다.
    ++themeRevision_;
    cache_.SetTheme(&theme_, themeRevision_);
}

void Application::SyncVectorRevision() noexcept {
    if (!vectorImages_ || vectorImages_->Revision() == vectorRevision_) {
        return;
    }

    vectorRevision_ = vectorImages_->Revision();
    ++themeRevision_;
    cache_.SetTheme(&theme_, themeRevision_);
}

Application::~Application() noexcept {
    if (context_) {
        *context_ = nullptr;
        context_.Reset();
    }

    ResetInput();
    DetachHostServices();
    if (host_) {
        // 공급자 delegate가 잡은 호스트 객체보다 Element가 오래 살 수 있다.
        host_->SetPopupWorkArea({});
        host_->SetTextInputService({});
    }
}

void Application::DetachHostServices() noexcept {
    if (root_ && host_ && root_->GetUiHostServices() == host_.Get()) {
        (void)root_->SetUiHostServices({});
    }
}

void Application::SetRoot(const SharedPtr<Element>& root) noexcept {
    ResetInput();
    DetachHostServices();
    root_ = root;
    // 묶음은 루트를 받을 때 만든다(20.3절 — Element가 프레임 시각을 읽는다). 실패하면 시각 0이다.
    (void)EnsureHostServices();
    cache_.Reset();
    hitGrid_.Reset();
    ApplyHostServices();
}

const SharedPtr<Element>& Application::GetRoot() const noexcept {
    return root_;
}

UiHostServices* Application::EnsureHostServices() noexcept {
    if (!host_) {
        auto made = SharedPtr<UiHostServices>::TryMake(allocator_);
        if (!made) {
            return nullptr;
        }

        host_ = Move(made.Value());
        host_->SetFrameTime(frameTime_);
        ApplyHostServices();
    }

    return host_.Get();
}

Result<void> Application::SetPopupWorkArea(Element::PopupWorkArea&& provider) noexcept {
    UiHostServices* host = EnsureHostServices();
    if (!host) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    host->SetPopupWorkArea(Move(provider));
    if (root_) {
        root_->InvalidateArrangement();
    }

    return Result<void>::Success();
}

double Application::ReadSourceClock() const noexcept {
    return clock_.IsBound()
               ? clock_.Execute()
               : static_cast<double>(platform::MonotonicNanoseconds()) * SecondsPerNanosecond;
}

double Application::ReadClock() const noexcept {
    return ReadSourceClock() + clockOffset_;
}

double Application::WaitClock() const noexcept {
    return externalTime_ ? frameTime_.current : ReadClock();
}

void Application::BeginFrame() noexcept {
    explicitFrames_ = true;
    const double now = ReadClock();
    if (!clockStarted_ || !(now >= lastClock_)) {
        // 첫 프레임이거나 시계가 거꾸로 갔다 — 간격 0에서 다시 센다.
        lastClock_ = now;
        clockStarted_ = true;
    }

    // UE TickTime: 시각은 시계 그대로, 간격만 자른다(앞 시각을 당긴다).
    if (now - lastClock_ > MaxFrameDeltaSeconds) {
        lastClock_ = now - MaxFrameDeltaSeconds;
    }

    StartFrame(FrameTime{now, now - lastClock_}, false);
    lastClock_ = now;
}

void Application::BeginFrame(const FrameTime& time) noexcept {
    explicitFrames_ = true;
    if (!math::IsFinite(time.current) || !math::IsFinite(time.delta)) {
        return;
    }

    StartFrame(FrameTime{time.current, math::Clamp(time.delta, 0.0, MaxFrameDeltaSeconds)}, true);
}

void Application::StartFrame(const FrameTime& time, bool external) noexcept {
    frameTime_ = time;
    externalTime_ = external;
    frameBegun_ = true;
    ++frameSerial_;
    if (host_) {
        host_->SetFrameTime(frameTime_);
    }
}

void Application::EnsureFrame() noexcept {
    if (explicitFrames_ || frameBegun_) {
        return;
    }

    // 암묵 시작 호스트다(BeginFrame을 부른 적이 없다) — 앱 시계로 연다.
    const double now = ReadClock();
    if (!clockStarted_ || !(now >= lastClock_)) {
        lastClock_ = now;
        clockStarted_ = true;
    }

    if (now - lastClock_ > MaxFrameDeltaSeconds) {
        lastClock_ = now - MaxFrameDeltaSeconds;
    }

    StartFrame(FrameTime{now, now - lastClock_}, false);
    lastClock_ = now;
}

const FrameTime& Application::GetFrameTime() const noexcept {
    return frameTime_;
}

void Application::SetClock(ClockFunction&& clock) noexcept {
    while (!toolTips_.IsEmpty()) {
        CloseToolTip(toolTips_[toolTips_.Size() - 1].user);
    }

    // 프레임을 연 뒤의 교체는 UI 시각을 이어 붙인다(20.3절 — 걸린 마감·부품 원점이 그대로 맞는다).
    const bool continuing = clockStarted_;
    const double ui = continuing ? lastClock_ : 0;
    clock_ = Move(clock);
    clockOffset_ = continuing ? ui - ReadSourceClock() : 0;
}

Result<void> Application::SetTextInputService(SharedPtr<TextInputService> service) noexcept {
    UiHostServices* host = EnsureHostServices();
    if (!host) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    host->SetTextInputService(Move(service));
    return Result<void>::Success();
}

bool Application::ProcessTextInput(uint32 record) noexcept {
    if (!host_ || !host_->TextInput()) {
        return false;
    }

    // 처리 중 서비스가 바뀌어도 이 호출 동안 살아 있게 쥔다.
    const SharedPtr<TextInputService> service = host_->TextInput();
    service->Process(record);
    return true;
}

void Application::ApplyHostServices() noexcept {
    if (root_ && host_) {
        // 실패(메모리 부족)하면 팝업은 창 경계만 지키고 IME 글자는 Text 이벤트로 온다.
        (void)root_->SetUiHostServices(host_);
    }
}

namespace {

void DismissPopupsBelow(Element& element) noexcept {
    element.DismissHostedPopups();
    for (usize i = 0; i < element.ChildCount(); ++i) {
        if (Element* child = element.ChildAt(i)) {
            DismissPopupsBelow(*child);
        }
    }
}

} // namespace

Result<uint64> Application::AddPressObserver(PressObserver&& observer) noexcept {
    if (!observer.IsBound()) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    // 알림 중에는 배열을 늘리지 않는다(실행 중인 delegate가 옮겨지지 않게).
    if (notifyingPress_) {
        return Result<uint64>::Failure(ErrorCode::Busy);
    }

    const uint64 id = nextPressObserver_++;
    auto stored = pressObservers_.TryPushBack({id, Move(observer), false});
    if (!stored) {
        return Result<uint64>::Failure(stored.Error());
    }

    return Result<uint64>::Success(id);
}

void Application::RemovePressObserver(uint64 id) noexcept {
    for (usize i = 0; i < pressObservers_.Size(); ++i) {
        if (pressObservers_[i].id != id) {
            continue;
        }

        // 알림 중이면 표시만 하고 알림이 끝난 뒤 지운다.
        if (notifyingPress_) {
            pressObservers_[i].removed = true;
        } else {
            (void)pressObservers_.TryRemoveAt(i);
        }

        return;
    }
}

bool Application::NotifyPressObservers(const PointerEvent& event, Element* hit) noexcept {
    if (pressObservers_.IsEmpty() || notifyingPress_) {
        return false;
    }

    notifyingPress_ = true;
    bool consumed = false;
    for (usize i = 0; i < pressObservers_.Size() && !consumed; ++i) {
        if (!pressObservers_[i].removed) {
            consumed = pressObservers_[i].observer.Execute(event, hit);
        }
    }

    notifyingPress_ = false;
    for (usize i = pressObservers_.Size(); i > 0; --i) {
        if (pressObservers_[i - 1].removed) {
            (void)pressObservers_.TryRemoveAt(i - 1);
        }
    }

    return consumed;
}

void Application::DismissPopups() noexcept {
    while (!toolTips_.IsEmpty()) {
        CloseToolTip(toolTips_[toolTips_.Size() - 1].user);
    }

    if (auto root = root_) {
        DismissPopupsBelow(*root);
    }
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

namespace {

// hit 대상 Element의 창 영역이다(QueryWindowRegion·BuildWindowRegionMap이 같은 규칙을 쓴다).
input::WindowRegion RegionOfTarget(const Element* target) noexcept {
    // 팝업 층 안 지점은 다른 판정보다 앞선다(직접 그린 창의 크기 조절 가장자리보다도 — UW7c R6).
    for (const Element* current = target; current && current->Parent();
         current = current->Parent()) {
        if (current->Parent()->IsHostedPopupLayer(*current)) {
            return input::WindowRegion::Popup;
        }
    }

    while (target) {
        const auto region = target->GetWindowRegion();
        if (region != input::WindowRegion::Client || target->SupportsKeyboardFocus()) {
            return region;
        }

        target = target->Parent();
    }

    return input::WindowRegion::Client;
}

// 창 좌표 사각형을 정수 client 사각형으로 바꾼다. outward면 바깥으로(구멍·Popup), 아니면 안으로.
IntRect ToClientRect(const Box2<float>& area, bool outward) noexcept {
    const auto lower = [outward](float value) noexcept {
        return static_cast<int32>(outward ? math::Floor(value) : math::Ceil(value));
    };
    const auto upper = [outward](float value) noexcept {
        return static_cast<int32>(outward ? math::Ceil(value) : math::Floor(value));
    };
    return {{lower(area.min.x), lower(area.min.y)}, {upper(area.max.x), upper(area.max.y)}};
}

bool Overlaps(const IntRect& a, const IntRect& b) noexcept {
    return a.min.x < b.max.x && b.min.x < a.max.x && a.min.y < b.max.y && b.min.y < a.max.y;
}

} // namespace

input::WindowRegion Application::QueryWindowRegion(Vec2 position) const noexcept {
    if (!root_) {
        return input::WindowRegion::Client;
    }

    const bool snapshot = hitGrid_.HasSnapshot();
    // 영역 판정은 구조라 활성을 보지 않는다(20.5절).
    const auto hit = snapshot ? hitGrid_.Find(position, *root_, true) : SharedPtr<Element>{};
    return RegionOfTarget(snapshot ? hit.Get() : root_->HitTest(position, true));
}

void Application::BuildWindowRegionMap(input::WindowRegionMap& map) const noexcept {
    map = {};
    if (!root_ || !hitGrid_.HasSnapshot()) {
        return;
    }

    // hit 후보를 위에서부터 보고 영역을 정한다(각 후보가 위에서 처음 걸리는 점의 답이다).
    struct Answer {
        IntRect rect;
        input::WindowRegion region;
    };
    Array<Answer> answers(allocator_);
    const usize count = hitGrid_.CandidateCount();
    if (!answers.TryReserve(count)) {
        return;
    }

    for (usize fromTop = 0; fromTop < count; ++fromTop) {
        Box2<float> area;
        const auto candidate = hitGrid_.CandidateFromTop(fromTop, area);
        if (!candidate) {
            continue;
        }

        const auto region = RegionOfTarget(candidate.Get());
        const bool outward =
            region == input::WindowRegion::Client || region == input::WindowRegion::Popup;
        const IntRect rect = ToClientRect(area, outward);
        if (!rect.IsEmpty()) {
            (void)answers.TryPushBack({rect, region});
        }

        map.popupOpen |= region == input::WindowRegion::Popup;
    }

    // Client가 아닌 영역과, 그 뒤(아래) 영역을 가리는 Client 구멍만 담는다(어느 항목에도 없으면
    // Client이므로 나머지 Client는 필요 없다).
    bool overflow = false;
    for (usize index = 0; index < answers.Size() && !overflow; ++index) {
        bool needed = answers[index].region != input::WindowRegion::Client;
        for (usize below = index + 1; below < answers.Size() && !needed; ++below) {
            needed = answers[below].region != input::WindowRegion::Client &&
                     Overlaps(answers[index].rect, answers[below].rect);
        }

        if (needed) {
            overflow = !map.TryAdd({answers[index].rect, answers[index].region});
        }
    }

    if (!overflow) {
        return;
    }

    // 넘치면 끌기(Caption)와 구멍을 빼고 누르는 영역만 남긴다(그 밖은 Client — 끌 수 없을 뿐 잘못
    // 누르지는 않는다).
    const bool popupOpen = map.popupOpen;
    map = {};
    map.popupOpen = popupOpen;
    for (const Answer& answer : answers) {
        if (answer.region != input::WindowRegion::Client &&
            answer.region != input::WindowRegion::Caption &&
            !map.TryAdd({answer.rect, answer.region})) {
            break;
        }
    }

    if (!regionMapOverflowLogged_) {
        regionMapOverflowLogged_ = true;
        Log::Warning(LogUiApplication,
                     "The window region map overflowed {Count} rects; caption dragging is "
                     "disabled until it fits",
                     input::MaxWindowRegionRects);
    }
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

    SyncVectorRevision();
    list.Clear();
    list.SetTheme(&theme_, themeRevision_);
    list.SetVectorImageResolver(vectorImages_);
    list.SetFrameTime(frameTime_);
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

    SyncVectorRevision();
    cache_.SetFrameTime(frameTime_);
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
    // 암묵 시작 호스트면 여기서 프레임을 연다(20.3절). 이 배치가 그 프레임을 닫는다.
    EnsureFrame();
    frameBegun_ = false;
    AdvanceTimers();
    AdvanceToolTips();
    SharedPtr<Element> root = root_;
    if (!root) {
        ReleaseInactiveTargets();
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    // 묶음(보이기 포함)을 먼저 평가한 뒤 숨거나 접힌 요소의 초점·capture·hover를 푼다(같은
    // 프레임에).
    root->UpdateBoundAttributes();
    ReleaseInactiveTargets();
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

FocusCause Application::GetFocusCause(uint32 userId) const noexcept {
    for (const auto& focus : focus_) {
        if (focus.userId == userId) {
            return focus.cause;
        }
    }

    return FocusCause::Direct;
}

Result<void> Application::SetFocus(uint32 userId, Element& target, FocusCause cause) noexcept {
    auto owned = root_.Get() == &target ? root_
                                        : (target.Parent() ? target.Parent()->FindChild(&target)
                                                           : SharedPtr<Element>{});
    return owned ? SetFocus(userId, owned, cause) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> Application::SetFocus(uint32 userId, const SharedPtr<Element>& target,
                                   FocusCause cause) noexcept {
    if (focusChanging_ || cancelling_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 키보드 툴팁 예약이 프레임 시각을 본다 — 암묵 시작 호스트면 프레임을 연다(20.3절).
    EnsureFrame();
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
    // 외곽선은 키보드·게임패드 이동으로 옮긴 초점에만 보인다(UE SetUserFocus의 ShowFocus).
    const bool outline = target && ResolveFocusOutline(*target, cause);
    if (previous.Get() == target.Get() && (target || index == focus_.Size())) {
        if (target) {
            const bool causeChanged = focus_[index].cause != cause;
            focus_[index].cause = cause;
            if (focus_[index].outline != outline) {
                focus_[index].outline = outline;
                target->SetFocusOutlineVisible(outline);
            }

            // 같은 대상이라도 원인이 Navigation으로 바뀌면 키보드 툴팁을 예약한다.
            if (causeChanged) {
                ObserveFocusToolTip(userId, target, cause);
            }
        }

        return Result<void>::Success();
    }

    const bool hadOutline = index < focus_.Size() && focus_[index].outline;
    if (target && index == focus_.Size()) {
        auto added = focus_.TryPushBack({userId, WeakPtr<Element>(target), cause, outline});
        if (!added) {
            return added;
        }
    } else if (target) {
        focus_[index] = {userId, WeakPtr<Element>(target), cause, outline};
    } else if (index < focus_.Size()) {
        (void)focus_.TryRemoveAt(index);
    }

    if (hadOutline && previous) {
        previous->SetFocusOutlineVisible(false);
    }

    if (outline) {
        target->SetFocusOutlineVisible(true);
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
    // 툴팁 층을 닫는 일은 트리를 바꾸므로 초점 알림이 모두 끝난 뒤에 한다.
    ObserveFocusToolTip(userId, target, cause);
    return Result<void>::Success();
}

bool Application::ResolveFocusOutline(const Element& target, FocusCause cause) const noexcept {
    // 초점 잎에서 루트 방향으로 처음 답한 요소를 따른다.
    for (const Element* node = &target; node; node = node->Parent()) {
        if (const Optional<bool> answer = node->OnQueryShowFocus(cause)) {
            return *answer;
        }
    }

    return cause == FocusCause::Navigation;
}

bool Application::RouteGamepadButtons(uint32 user, uint32 pressed, uint32 released,
                                      const SharedPtr<Element>& scope) noexcept {
    EnsureFrame();
    // 키보드 KeyDown처럼 버튼 누름도 열린 툴팁을 닫는다.
    if (pressed != 0) {
        CloseToolTip(user);
    }

    const NavigationConfig& config = GetNavigationConfig(user);
    KeyChord chord;
    chord.shift = IsShiftDown(user);
    chord.control = IsControlDown(user);
    chord.alt = IsAltDown(user);
    bool handled = false;
    for (uint32 i = 0; i < input::GamepadButtonCount; ++i) {
        const uint32 bit = uint32{1} << i;
        if (!((pressed | released) & bit)) {
            continue;
        }

        const auto button = static_cast<input::GamepadButton>(i);
        const bool down = (pressed & bit) != 0;
        const auto event = down ? input::InputEvent::KeyDown(input::Key::None)
                                : input::InputEvent::KeyUp(input::Key::None);
        chord.gamepad = button;
        const KeyEvent key{user, event, chord,
                           down ? config.DirectionFromGamepad(button) : Optional<FocusDirection>{},
                           config.ActionFromGamepad(button)};
        auto routed = RouteKeyEvent(key, scope);
        handled = handled || (routed && routed.Value());
    }

    return handled;
}

Result<bool> Application::RouteGamepadState(uint32 user, uint32 index,
                                            const input::GamepadSnapshot& pad,
                                            const SharedPtr<Element>& scope) noexcept {
    if (index >= input::MaxGamepads) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    EnsureFrame();
    PadState& state = pads_[index];
    const uint32 buttons = pad.connected ? pad.buttons : 0;
    const uint32 pressed = buttons & ~state.buttons;
    const uint32 released = state.buttons & ~buttons;
    state.buttons = buttons;
    bool handled = (pressed | released) && RouteGamepadButtons(user, pressed, released, scope);

    // 왼쪽 스틱: 임계값을 넘는 순간 한 번, 그 뒤 반복 간격마다(UE GetNavigationDirectionFromAnalog
    // 83–105행). 간격은 프레임 시각으로 잰다(20.3절).
    const NavigationConfig& config = GetNavigationConfig(user);
    const float x = pad.connected ? pad.axes[static_cast<uint32>(input::GamepadAxis::LeftX)] : 0;
    const float y = pad.connected ? pad.axes[static_cast<uint32>(input::GamepadAxis::LeftY)] : 0;
    const auto direction = config.DirectionFromAnalog(x, y);
    if (!direction) {
        state.analog = {};
        state.analogRepeats = 0;
        return Result<bool>::Success(handled);
    }

    const double now = frameTime_.current;
    const float pressure = math::Max(x < 0 ? -x : x, y < 0 ? -y : y);
    const bool fresh = state.analog != direction;
    if (!fresh && now - state.analogLast <=
                      NavigationConfig::AnalogRepeatInterval(pressure, state.analogRepeats - 1)) {
        return Result<bool>::Success(handled);
    }

    if (fresh) {
        state.analog = direction;
        state.analogRepeats = 0;
    }

    state.analogLast = now;
    ++state.analogRepeats;
    // 스틱 navigation은 키 없는 navigation 이벤트로 초점 경로에 보낸다(초점 요소가 쓸 수 있다).
    const auto event = input::InputEvent::KeyDown(input::Key::None);
    const KeyEvent key{user, event, {}, direction, NavigationAction::None};
    auto routed = RouteKeyEvent(key, scope);
    if (!routed) {
        return routed;
    }

    return Result<bool>::Success(handled || routed.Value());
}

void Application::SyncGamepadState(uint32 index, const input::GamepadSnapshot& pad) noexcept {
    if (index >= input::MaxGamepads) {
        return;
    }

    PadState& state = pads_[index];
    state.buttons = pad.connected ? pad.buttons : 0;
    const NavigationConfig& config = NavigationConfig::Default();
    const auto direction =
        pad.connected
            ? config.DirectionFromAnalog(pad.axes[static_cast<uint32>(input::GamepadAxis::LeftX)],
                                         pad.axes[static_cast<uint32>(input::GamepadAxis::LeftY)])
            : Optional<FocusDirection>{};
    // 밀고 있던 스틱은 이어지는 방향으로 본다(반복 간격부터 센다).
    if (direction != state.analog) {
        state.analog = direction;
        state.analogRepeats = direction ? 1 : 0;
        state.analogLast = frameTime_.current;
    }
}

void Application::ResetGamepads() noexcept {
    for (auto& pad : pads_) {
        pad = {};
    }
}

Result<void> Application::SetNavigationConfig(uint32 userId,
                                              const NavigationConfig& config) noexcept {
    for (auto& entry : navigationConfigs_) {
        if (entry.user == userId) {
            entry.config = config;
            return Result<void>::Success();
        }
    }

    return navigationConfigs_.TryPushBack({userId, config});
}

const NavigationConfig& Application::GetNavigationConfig(uint32 userId) const noexcept {
    for (const auto& entry : navigationConfigs_) {
        if (entry.user == userId) {
            return entry.config;
        }
    }

    return NavigationConfig::Default();
}

Result<bool> Application::PerformNavigation(uint32 userId, FocusDirection direction,
                                            const SharedPtr<Element>& scope) noexcept {
    constexpr FocusCause cause = FocusCause::Navigation;
    if (direction == FocusDirection::Next || direction == FocusDirection::Previous) {
        const bool forward = direction == FocusDirection::Next;
        // 팝업·modal 범위 안의 Tab은 범위 안에서 돈다.
        return scope ? MoveFocusWithin(userId, scope, forward, true, cause)
                     : MoveFocus(userId, forward, false, cause);
    }

    return Navigate(userId, direction, scope, cause);
}

Result<bool> Application::RouteKey(uint32 userId, const input::InputEvent& event) noexcept {
    return RouteKeyWithin(userId, event, {});
}

Result<bool> Application::RouteKeyWithin(uint32 userId, const input::InputEvent& event,
                                         const SharedPtr<Element>& scope) noexcept {
    EnsureFrame();
    if (event.type != input::InputEventType::KeyDown &&
        event.type != input::InputEventType::KeyUp && event.type != input::InputEventType::Text) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    // 보조 키만 누를 때는 닫지 않는다(Shift·Ctrl을 누른 채 툴팁을 읽을 수 있게 — UE는 닫는다).
    if (event.type == input::InputEventType::KeyDown && !IsModifierKey(event.key)) {
        CloseToolTip(userId);
    }

    auto chord = UpdateChord(userId, event);
    if (!chord) {
        return Result<bool>::Failure(chord.Error());
    }

    // 키마다 navigation 방향·동작을 사용자의 대응으로 미리 계산한다(Element는 Application을
    // 모른다).
    const NavigationConfig& config = GetNavigationConfig(userId);
    const KeyEvent key{userId, event, chord.Value(), config.DirectionFromKey(event, chord.Value()),
                       config.ActionFromKey(event)};
    return RouteKeyEvent(key, scope);
}

Result<bool> Application::RouteKeyEvent(const KeyEvent& key,
                                        const SharedPtr<Element>& scope) noexcept {
    const uint32 userId = key.user;
    const input::InputEvent& event = key.input;
    const bool down = event.type == input::InputEventType::KeyDown;
    SharedPtr<Element> target = GetFocus(userId);
    if (!target || !IsFocusablePath(target.Get())) {
        auto cleared = SetFocus(userId, {});
        if (!cleared) {
            return Result<bool>::Failure(cleared.Error());
        }

        return Result<bool>::Success(down && !scope &&
                                     ProcessCommands(nullptr, key.chord, event.repeat));
    }

    Array<SharedPtr<Element>> path(allocator_);
    if (scope && !target->IsWithin(*scope)) {
        return Result<bool>::Success(false);
    }

    auto pathResult = BuildPath(target.Get(), scope ? scope : root_, path);
    if (!pathResult) {
        return Result<bool>::Failure(pathResult.Error());
    }

    if (down) {
        for (usize i = path.Size(); i > 0; --i) {
            if (path[i - 1]->OnPreviewKeyChord(userId, event, key.chord)) {
                return Result<bool>::Success(true);
            }
        }
    }

    for (usize i = 0; i < path.Size(); ++i) {
        if (down && ProcessCommands(path[i].Get(), key.chord, event.repeat)) {
            return Result<bool>::Success(true);
        }

        const KeyReply reply = path[i]->OnKeyEvent(key);
        if (const auto& direction = reply.Navigation()) {
            // navigation 응답이면 라우팅을 멈추고(조상은 키를 받지 않는다) 탐색한다. 찾지 못해도
            // 키는 처리된 것이다(UE FReply::SetNavigation).
            auto navigated = PerformNavigation(userId, *direction, scope);
            return navigated ? Result<bool>::Success(true) : Move(navigated);
        }

        if (reply.IsHandled()) {
            return Result<bool>::Success(true);
        }
    }

    return Result<bool>::Success(down && !scope &&
                                 ProcessCommands(nullptr, key.chord, event.repeat));
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
    EnsureFrame();
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
        // 툴팁은 비활성 요소도 띄운다 — 활성을 무시한 대상으로 본다(UE SlateUser.cpp 1351행).
        SharedPtr<Element> tipRecorded;
        Element* tipHit = hit;
        if (!captured) {
            if (hitGrid_.HasSnapshot()) {
                tipRecorded = hitGrid_.Find(event.position, *root, true);
                tipHit = tipRecorded.Get();
            } else {
                tipHit = root->HitTest(event.position, true);
            }
        }

        ObserveToolTip(event, tipHit, root);
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

    // 새 누름은 관찰자(메뉴 막대의 바깥 누름 닫기 등)가 먼저 본다. 트리를 바꾼 관찰자는 누름을
    // 소비해야 한다(맞은 요소가 트리에서 떨어졌을 수 있다).
    if (event.type == PointerEventType::Down && !captured && NotifyPressObservers(event, hit)) {
        return Result<bool>::Success(true);
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
        bool chosen = false;
        for (usize i = 0; i < path.Size(); ++i) {
            if (path[i]->SupportsKeyboardFocus() && IsFocusablePath(path[i].Get())) {
                auto focused = SetFocus(event.userId, path[i], FocusCause::Pointer);
                if (!focused) {
                    return Result<bool>::Failure(focused.Error());
                }

                chosen = true;
                break;
            }
        }

        // 빈 곳을 눌러도 초점은 두되 마우스 사용으로 바뀌었으니 외곽선은 끈다(UE는 창이 초점을
        // 가져가 외곽선이 사라진다 — SWindow.cpp 1783행. Rocks는 편집 중인 값의 확정을 피하려고
        // 초점은 유지한다).
        // 초점 요소를 다시 정하지 않고(누름 중 떨어져 나갔거나 초점을 못 받게 됐을 수 있다) 원인을
        // Pointer로 바꾸고 외곽선만 끈다 — 외형 정리라 실패할 길이 없다.
        if (!chosen) {
            for (auto& entry : focus_) {
                if (entry.userId != event.userId) {
                    continue;
                }

                entry.cause = FocusCause::Pointer;
                if (entry.outline) {
                    entry.outline = false;
                    if (auto current = entry.target.Lock()) {
                        current->SetFocusOutlineVisible(false);
                    }
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
        const bool outline = focus_[0].outline;
        (void)focus_.TryRemoveAt(0);
        if (previous && outline) {
            previous->SetFocusOutlineVisible(false);
        }

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
        const bool outline = focus_[i].outline;
        (void)focus_.TryRemoveAt(i);
        ++focusVersion_;
        if (target && outline) {
            target->SetFocusOutlineVisible(false);
        }

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

Result<bool> Application::MoveFocus(uint32 userId, bool forward, bool wrap,
                                    FocusCause cause) noexcept {
    return MoveFocusWithin(userId, root_, forward, wrap, cause);
}

Result<bool> Application::MoveFocusWithin(uint32 userId, const SharedPtr<Element>& root,
                                          bool forward, bool wrap, FocusCause cause) noexcept {
    return root
               ? NavigateInternal(userId, forward ? FocusDirection::Next : FocusDirection::Previous,
                                  root, wrap, cause)
               : Result<bool>::Success(false);
}

Result<void> Application::CollectFocusTargets(const SharedPtr<Element>& element,
                                              Array<SharedPtr<Element>>& targets) const noexcept {
    // 누를 수 없는 요소는 탐색 대상도 아니다(UE 탐색은 hit grid 기반). HitTestInvisible은 하위까지,
    // SelfHitTestInvisible은 자기만 빠진다 — 툴팁 층이 Tab 순서에 끼지 않는다.
    const Visibility visibility = element->GetVisibility();
    if (!element->IsEnabled() || visibility == Visibility::Collapsed ||
        visibility == Visibility::Hidden || visibility == Visibility::HitTestInvisible) {
        return Result<void>::Success();
    }

    if (element->SupportsKeyboardFocus() && visibility != Visibility::SelfHitTestInvisible) {
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
