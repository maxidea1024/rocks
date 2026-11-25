#include <UiFramework/Application.h>
#include <UiFramework/Overlay.h>
#include <Platform/Clock.h>

namespace rocks::ui {
namespace {
// 호스트 Overlay의 전체 크기를 받되 내용만 커서 또는 소유 요소 옆에 배치한다.
class ToolTipLayer final : public Element {
public:
    ToolTipLayer(Allocator& allocator, Vec2 point, const SharedPtr<Element>& owner,
                 bool interactive) noexcept
        : Element(allocator), point_(point), owner_(owner), interactive_(interactive) {
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
        auto local = inverse.Apply(point_);
        const auto owner = owner_.Lock();
        if (interactive_ && owner) {
            auto bounds = Geometry::Root(owner->GetGeometry().size,
                                         inverse.Compose(owner->GetGeometry().render))
                              .WindowBounds();
            if (!bounds) {
                return Result<void>::Failure(bounds.Error());
            }
            local = {bounds.Value().min.x, bounds.Value().max.y};
        } else {
            local.x += 12;
            local.y += 16;
        }
        const auto desired = child->GetDesiredSize();
        const Vec2 size{math::Min(area.size.x, desired.x), math::Min(area.size.y, desired.y)};
        const float x = math::Clamp(local.x, 0.f, area.size.x - size.x);
        const float y = math::Clamp(local.y, 0.f, area.size.y - size.y);
        return child->ArrangeIfNeeded(area.MakeChild(size, {1, 0, 0, 1, x, y}, {}, {}));
    }

private:
    Vec2 point_;
    WeakPtr<Element> owner_;
    bool interactive_;
};
} // namespace

double Application::ToolTipTime() const noexcept {
    return toolTipClock_ ? toolTipClock_()
                         : static_cast<double>(platform::MonotonicNanoseconds()) * 1.e-9;
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
        if (owner && state.owner.Lock().Get() == owner &&
            state.revision == owner->ToolTipRevision()) {
            state.point = event.position;
            state.leaveDue = 0;
            return;
        }
        auto previous = state.owner.Lock();
        if (!owner && state.layer && previous && previous->IsToolTipInteractive()) {
            if (state.leaveDue == 0) {
                state.leaveDue = ToolTipTime() + 0.15;
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
                                         false});
        }
    }
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
    cache_.Reset();
    hitGrid_.Reset();
    return Result<void>::Success();
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
                                                              owner->IsToolTipInteractive());
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
