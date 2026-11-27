#include <UiCore/TimeUnits.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Overlay.h>
#include <Platform/Clock.h>

namespace rocks::ui {

namespace {

// 커서 기준 툴팁 위치다(커서 그림을 피하는 오른쪽 아래 — UE SlateApplication.cpp 619행
// ToolTipOffsetFromMouse(12, 8)은 5.8에서 쓰이지 않아 이전 Rocks 값을 유지한다).
constexpr Vec2 ToolTipCursorOffset{12, 16};
// 키보드·대화형 툴팁과 소유자 사이 간격이다.
constexpr float ToolTipOwnerGap = 2;
// 대화형 툴팁에서 포인터가 나간 뒤 닫기까지의 유예(초)다.
constexpr double InteractiveToolTipLeaveDelay = 0.15;

// 호스트 Overlay의 전체 크기를 받되 내용만 커서 또는 소유 요소 옆에 배치한다.
class ToolTipLayer final : public Element {
public:
    ToolTipLayer(Allocator& allocator, Vec2 point, const SharedPtr<Element>& owner,
                 bool interactive, bool belowOwner) noexcept
        : Element(allocator), point_(point), owner_(owner), interactive_(interactive),
          belowOwner_(belowOwner) {
        SetVisibility(interactive ? Visibility::SelfHitTestInvisible
                                  : Visibility::HitTestInvisible);
    }

    Vec2 ComputeDesiredSize(float) const noexcept override {
        return {};
    }

    Result<void> Arrange(const Geometry& area) noexcept override {
        SetGeometry(area);
        auto* child = ChildAt(0);
        if (!child) {
            return Result<void>::Success();
        }

        AffineTransform2D inverse;
        if (!area.render.TryInverse(inverse)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 배치 경계는 호스트가 알린 팝업 허용 영역(UW7d)이고, 없으면 층 전체다. 그보다 넓으면 그
        // 폭에서 줄을 다시 나눈 높이를 같은 프레임에 쓴다.
        const Vec2 areaMin = popupArea_ ? popupArea_->min : Vec2{};
        const Vec2 areaMax = popupArea_ ? popupArea_->max : area.size;
        const Vec2 areaSize{math::Max(0.0f, areaMax.x - areaMin.x),
                            math::Max(0.0f, areaMax.y - areaMin.y)};
        Vec2 desired = child->GetDesiredSize();
        if (desired.x > areaSize.x) {
            desired = child->MeasureForWidth(areaSize.x);
        }

        const Vec2 size{math::Min(areaSize.x, desired.x), math::Min(areaSize.y, desired.y)};
        Vec2 local;
        const auto owner = owner_.Lock();
        if ((interactive_ || belowOwner_) && owner) {
            // 소유자 아래 왼쪽, 아래가 모자라면 소유자 위로 뒤집는다(소유자를 덮지 않는다).
            auto bounds = Geometry::Root(owner->GetGeometry().size,
                                         inverse.Compose(owner->GetGeometry().render))
                              .WindowBounds();
            if (!bounds) {
                return Result<void>::Failure(bounds.Error());
            }

            const float gap = belowOwner_ ? ToolTipOwnerGap : 0.0f;
            local = {bounds.Value().min.x, bounds.Value().max.y + gap};
            if (local.y + size.y > areaMax.y) {
                local.y = bounds.Value().min.y - gap - size.y;
            }
        } else {
            // 커서 오른쪽 아래, 아래가 모자라면 커서 위로 뒤집는다(UE
            // CalculateTooltipWindowPosition처럼 커서를 덮지 않는다).
            const Vec2 cursor = inverse.Apply(point_);
            local = cursor + ToolTipCursorOffset;
            if (local.y + size.y > areaMax.y) {
                local.y = cursor.y - size.y;
            }
        }

        const float x = math::Clamp(local.x, areaMin.x, math::Max(areaMin.x, areaMax.x - size.x));
        const float y = math::Clamp(local.y, areaMin.y, math::Max(areaMin.y, areaMax.y - size.y));
        return child->ArrangeIfNeeded(area.MakeChild(size, {1, 0, 0, 1, x, y}, {}, {}));
    }

    // 커서에 붙는 툴팁은 커서가 놓인 모니터 안에 놓는다(UE CalculateTooltipWindowPosition). 소유자
    // 아래 툴팁(키보드·상호작용)은 소유자의 왼쪽 위를 쓴다(호스트 기본).
    void SetPopupArea(const Optional<Box2<float>>& area) noexcept override {
        const bool same = area.HasValue() == popupArea_.HasValue() &&
                          (!area || (area->min == popupArea_->min && area->max == popupArea_->max));
        if (!same) {
            popupArea_ = area;
            InvalidateArrangement();
        }
    }

    Optional<Vec2> PopupWorkAreaPoint() const noexcept override {
        if (interactive_ || belowOwner_) {
            return {};
        }

        return point_;
    }

private:
    Vec2 point_;
    Optional<Box2<float>> popupArea_;
    WeakPtr<Element> owner_;
    bool interactive_;
    // 키보드 초점 툴팁은 커서가 아니라 소유자 아래 왼쪽에 놓는다.
    bool belowOwner_;
};
} // namespace

double Application::ToolTipTime() const noexcept {
    return toolTipClock_
               ? toolTipClock_()
               : static_cast<double>(platform::MonotonicNanoseconds()) * SecondsPerNanosecond;
}

usize Application::FindToolTip(uint32 user) const noexcept {
    for (usize i = 0; i < toolTips_.Size(); ++i) {
        if (toolTips_[i].user == user) {
            return i;
        }
    }

    return toolTips_.Size();
}

void Application::SetToolTipClock(double (*clock)() noexcept) noexcept {
    while (!toolTips_.IsEmpty()) {
        CloseToolTip(toolTips_[toolTips_.Size() - 1].user);
    }

    toolTipClock_ = clock;
}

Result<void> Application::SetToolTipTiming(double delay, double fade) noexcept {
    if (!math::IsFinite(delay) || !math::IsFinite(fade) || delay < 0 || fade < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    toolTipDelay_ = delay;
    toolTipFade_ = fade;
    return Result<void>::Success();
}

double Application::ToolTipDelay() const noexcept {
    return toolTipDelay_;
}

double Application::ToolTipFade() const noexcept {
    return toolTipFade_;
}

bool Application::IsToolTipOpen(uint32 user) const noexcept {
    auto i = FindToolTip(user);
    return i < toolTips_.Size() && toolTips_[i].layer && toolTips_[i].layer->Parent();
}

void Application::CloseToolTip(uint32 user) noexcept {
    auto i = FindToolTip(user);
    if (i == toolTips_.Size()) {
        return;
    }

    auto layer = Move(toolTips_[i].layer);
    (void)toolTips_.TryRemoveAt(i);
    if (layer) {
        if (auto* parent = layer->Parent()) {
            (void)parent->RemoveChild(*layer);
        }

        if (auto* content = layer->ChildAt(0)) {
            (void)layer->RemoveChild(*content);
        }
    }

    if (auto host = toolTipHost_.Lock(); host && host == root_ && host->ChildCount() == 1) {
        auto original = host->ChildRefAt(0);
        (void)host->RemoveChild(*original);
        root_ = Move(original);
        toolTipHost_.Reset();
        cache_.Reset();
        hitGrid_.Reset();
    }
}

void Application::ObserveToolTip(const PointerEvent& event, Element* hit,
                                 const SharedPtr<Element>& scope) noexcept {
    const auto user = event.userId;
    auto i = FindToolTip(user);
    if (event.type != PointerEventType::Move) {
        bool interacting =
            i < toolTips_.Size() && hit && toolTips_[i].layer && hit->IsWithin(*toolTips_[i].layer);
        if (!interacting || event.type == PointerEventType::Cancel) {
            CloseToolTip(user);
        }

        return;
    }

    if (i < toolTips_.Size() && hit && toolTips_[i].layer && hit->IsWithin(*toolTips_[i].layer)) {
        toolTips_[i].leaveDue = 0;
        return;
    }

    Element* owner = hit;
    while (owner && !owner->HasToolTip()) {
        owner = owner == scope.Get() ? nullptr : owner->Parent();
    }

    if (i < toolTips_.Size()) {
        auto& state = toolTips_[i];
        // 키보드 툴팁은 다른 툴팁 소유자에 올라갈 때만 hover 툴팁으로 바뀐다(포인터 흔들림으로 닫지
        // 않는다).
        if (state.keyboard && (!owner || state.owner.Lock().Get() == owner)) {
            return;
        }

        if (owner && state.owner.Lock().Get() == owner &&
            state.revision == owner->ToolTipRevision()) {
            state.point = event.position;
            state.leaveDue = 0;
            return;
        }

        auto previous = state.owner.Lock();
        if (!owner && state.layer && previous && previous->IsToolTipInteractive()) {
            if (state.leaveDue == 0) {
                state.leaveDue = ToolTipTime() + InteractiveToolTipLeaveDelay;
            }

            return;
        }

        CloseToolTip(user);
    }

    if (owner) {
        auto ref = owner == scope.Get() ? scope
                                        : (owner->Parent() ? owner->Parent()->FindChild(owner)
                                                           : SharedPtr<Element>{});
        if (ref) {
            (void)toolTips_.TryPushBack({user,
                                         WeakPtr<Element>(ref),
                                         {},
                                         event.position,
                                         ToolTipTime() + toolTipDelay_,
                                         0,
                                         0,
                                         owner->ToolTipRevision(),
                                         false,
                                         false});
        }
    }
}

SharedPtr<Element> Application::FindToolTipOwner(const SharedPtr<Element>& focus) const noexcept {
    // hover 경로처럼 초점 잎에서 루트 방향으로 처음 툴팁을 가진 요소다(편집 칸을 담은 상자의 툴팁).
    if (!focus || !root_) {
        return {};
    }

    for (Element* node = focus.Get(); node; node = node->Parent()) {
        if (node->HasToolTip()) {
            if (node == focus.Get()) {
                return focus;
            }

            return node == root_.Get()
                       ? root_
                       : (node->Parent() ? node->Parent()->FindChild(node) : SharedPtr<Element>{});
        }

        if (node == root_.Get()) {
            break;
        }
    }

    return {};
}

bool Application::ShowFocusToolTip(uint32 user) noexcept {
    auto target = GetFocus(user);
    auto owner =
        target && IsFocusablePath(target.Get()) ? FindToolTipOwner(target) : SharedPtr<Element>{};
    if (!owner) {
        return false;
    }

    CloseToolTip(user);
    auto added = toolTips_.TryPushBack({user,
                                        WeakPtr<Element>(owner),
                                        {},
                                        Vec2{},
                                        ToolTipTime(),
                                        0,
                                        0,
                                        owner->ToolTipRevision(),
                                        false,
                                        true});
    return static_cast<bool>(added);
}

void Application::ObserveFocusToolTip(uint32 user, const SharedPtr<Element>& target,
                                      FocusCause cause) noexcept {
    // 초점이 키보드 툴팁 소유자 밖으로 나가면 닫는다.
    const usize i = FindToolTip(user);
    if (i < toolTips_.Size() && toolTips_[i].keyboard) {
        auto owner = toolTips_[i].owner.Lock();
        if (!owner || !target || !target->IsWithin(*owner)) {
            CloseToolTip(user);
        }
    }

    // UE는 툴팁을 마우스 hover로만 연다. Rocks는 키보드·게임패드로 옮긴 초점에도 같은 지연 뒤에
    // 연다(9.1절 UW4t — 키보드 사용자도 설명을 볼 수 있게). 소유자는 hover처럼 조상까지 찾는다.
    auto owner =
        target && cause == FocusCause::Navigation ? FindToolTipOwner(target) : SharedPtr<Element>{};
    if (!owner) {
        return;
    }

    const usize open = FindToolTip(user);
    if (open < toolTips_.Size() && toolTips_[open].keyboard &&
        toolTips_[open].owner.Lock() == owner) {
        return;
    }

    CloseToolTip(user);
    (void)toolTips_.TryPushBack({user,
                                 WeakPtr<Element>(owner),
                                 {},
                                 Vec2{},
                                 ToolTipTime() + toolTipDelay_,
                                 0,
                                 0,
                                 owner->ToolTipRevision(),
                                 false,
                                 true});
}

Result<void> Application::AttachToolTip(const PopupRequest& popup) noexcept {
    auto added = popup.anchor->AttachPopupLayer(popup);
    if (added || added.Error() != ErrorCode::NotFound || !root_ || root_->Parent()) {
        return added;
    }

    // 캔버스나 단일 컨트롤 자체가 창의 루트여도 팝업은 측정과 클리핑을 벗어나야 한다.
    // 첫 팝업에서만 투명 호스트를 만들고 기존 루트와 그 자식의 주소는 그대로 둔다.
    auto host = SharedPtr<Overlay>::TryMake(allocator_, allocator_);
    if (!host) {
        return Result<void>::Failure(host.Error());
    }

    host.Value()->SetVisibility(Visibility::SelfHitTestInvisible);
    auto attached = host.Value()->TryAddChild(root_);
    if (!attached) {
        return attached;
    }

    added = host.Value()->AttachPopupLayer(popup);
    if (!added) {
        (void)host.Value()->RemoveChild(*root_);
        return added;
    }

    root_ = host.Value();
    toolTipHost_ = WeakPtr<Element>(root_);
    ApplyPopupWorkArea();
    cache_.Reset();
    hitGrid_.Reset();
    return Result<void>::Success();
}

double Application::SecondsUntilNextToolTip() const noexcept {
    double wait = NoTimerDeadline;
    const double now = ToolTipTime();
    for (const ToolTipState& state : toolTips_) {
        if (!state.attempted) {
            wait = math::Min(wait, math::Max(0.0, state.due - now));
        }

        if (state.layer && toolTipFade_ > 0 && now - state.opened < toolTipFade_) {
            return 0;
        }

        if (state.leaveDue != 0) {
            wait = math::Min(wait, math::Max(0.0, state.leaveDue - now));
        }
    }

    return wait;
}

void Application::AdvanceToolTips() noexcept {
    const double now = ToolTipTime();
    for (usize i = 0; i < toolTips_.Size();) {
        const uint32 user = toolTips_[i].user;
        auto owner = toolTips_[i].owner.Lock();
        if (!root_ || !owner || !owner->IsInputPathActive(*root_, true) || !owner->HasToolTip() ||
            owner->ToolTipRevision() != toolTips_[i].revision ||
            (toolTips_[i].leaveDue != 0 && now >= toolTips_[i].leaveDue)) {
            CloseToolTip(user);
            continue;
        }

        if (!toolTips_[i].attempted && now >= toolTips_[i].due) {
            toolTips_[i].attempted = true;
            const Vec2 point = toolTips_[i].point;
            auto tip = owner->MakeToolTip();
            // 사용자 팩토리가 입력을 초기화하거나 화면을 교체했을 수 있다.
            auto current = FindToolTip(user);
            if (current == toolTips_.Size() || current != i || toolTips_[i].owner.Lock() != owner) {
                continue;
            }

            if (owner->ToolTipRevision() != toolTips_[i].revision || !root_ ||
                !owner->IsInputPathActive(*root_, true)) {
                CloseToolTip(user);
                continue;
            }

            if (tip && tip.Value() && !tip.Value()->Parent() && !owner->IsWithin(*tip.Value())) {
                auto layer = SharedPtr<ToolTipLayer>::TryMake(allocator_, allocator_, point, owner,
                                                              owner->IsToolTipInteractive(),
                                                              toolTips_[i].keyboard);
                if (layer && layer.Value()->TryAddChild(tip.Value())) {
                    auto attached = AttachToolTip({layer.Value(), owner, [](Element&) noexcept {}});
                    if (attached) {
                        toolTips_[i].layer = Move(layer.Value());
                        toolTips_[i].opened = now;
                    }
                }
            }
        }

        if (toolTips_[i].layer) {
            const float alpha = toolTipFade_ == 0
                                    ? 1.f
                                    : static_cast<float>(math::Clamp(
                                          (now - toolTips_[i].opened) / toolTipFade_, 0.0, 1.0));
            (void)toolTips_[i].layer->SetRenderOpacity(alpha);
        }

        ++i;
    }
}
} // namespace rocks::ui
