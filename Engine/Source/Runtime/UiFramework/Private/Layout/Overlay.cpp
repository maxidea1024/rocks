#include <Base/NumericLimits.h>
#include <UiFramework/Layout/Overlay.h>

namespace rocks::ui {

Overlay::Overlay(Allocator& allocator) noexcept
    : Element(allocator), popups_(allocator), orders_(allocator) {
}

Result<void> Overlay::TryAddChild(const SharedPtr<Element>& child) noexcept {
    return TryAddSlot(child, orders_.IsEmpty() ? 0 : orders_[orders_.Size() - 1]);
}

Result<void> Overlay::TryAddSlot(const SharedPtr<Element>& child, int32 zOrder) noexcept {
    usize index = 0;
    while (index < orders_.Size() && orders_[index] <= zOrder) {
        ++index;
    }

    auto stored = orders_.TryInsert(zOrder, index);
    if (!stored) {
        return stored;
    }

    auto added = TryInsertChild(index, child);
    if (!added) {
        (void)orders_.TryRemoveAt(index);
    }

    return added;
}

Result<void> Overlay::AttachPopupLayer(const PopupRequest& popup) noexcept {
    if (dismissingPopups_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    auto outer = Element::AttachPopupLayer(popup);
    if (outer || outer.Error() != ErrorCode::NotFound) {
        return outer;
    }

    if (!popup.layer || !popup.anchor || !popup.close || !popup.anchor->IsWithin(*this)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto stored =
        popups_.TryPushBack({WeakPtr<Element>(popup.anchor), popup.layer.Get(), popup.close});
    if (!stored) {
        return stored;
    }

    auto attached = TryAddSlot(popup.layer, NumericLimits<int32>::Max());
    if (!attached) {
        (void)popups_.TryRemoveAt(popups_.Size() - 1);
    }

    return attached;
}

bool Overlay::IsHostedPopupLayer(const Element& child) const noexcept {
    for (const OpenPopup& popup : popups_) {
        if (popup.layer == &child) {
            return true;
        }
    }

    return false;
}

void Overlay::DismissHostedPopups() noexcept {
    if (dismissingPopups_) {
        return;
    }

    dismissingPopups_ = true;
    while (!popups_.IsEmpty()) {
        const usize i = popups_.Size() - 1;
        auto anchor = popups_[i].anchor.Lock();
        const auto close = popups_[i].close;
        (void)popups_.TryRemoveAt(i);
        if (anchor) {
            close(*anchor);
        }
    }

    dismissingPopups_ = false;
}

void Overlay::OnDescendantDetached(const Element& subtree) noexcept {
    // close가 중첩 메뉴를 함께 닫을 수 있으므로 현재 항목을 먼저 제거하고 크기를 다시 읽는다.
    usize limit = popups_.Size();
    while (limit && !popups_.IsEmpty()) {
        const usize i = (limit < popups_.Size() ? limit : popups_.Size()) - 1;
        limit = i;
        auto anchor = popups_[i].anchor.Lock();
        if (!anchor || popups_[i].layer == &subtree || anchor->IsWithin(subtree)) {
            const auto close = popups_[i].close;
            (void)popups_.TryRemoveAt(i);
            if (anchor) {
                close(*anchor);
            }
        }
    }
}

bool Overlay::RemoveChild(Element& child) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i) == &child) {
            (void)orders_.TryRemoveAt(i);
            return Element::RemoveChild(child);
        }
    }

    return false;
}

bool Overlay::StartsPaintLayer(usize, usize) const noexcept {
    return true;
}

Vec2 Overlay::ComputeDesiredSize(float) const noexcept {
    Vec2 size{};
    for (usize i = 0; i < ChildCount(); ++i) {
        const Element* child = ChildAt(i);
        if (child->GetVisibility() != Visibility::Collapsed) {
            const Vec2 desired = child->GetDesiredSize();
            size.x = math::Max(size.x, desired.x);
            size.y = math::Max(size.y, desired.y);
        }
    }

    return size;
}

Result<void> Overlay::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    for (usize i = 0; i < ChildCount(); ++i) {
        Element* child = ChildAt(i);
        if (child->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        auto arranged = child->ArrangeIfNeeded(PopupLayerGeometry(*child));
        if (!arranged) {
            return arranged;
        }
    }

    return Result<void>::Success();
}

Geometry Overlay::PopupLayerGeometry(const Element& child) const noexcept {
    // 팝업 층이 아니거나 허용 영역을 모르면 Overlay 전체다.
    const Geometry& host = GetGeometry();
    const Geometry whole = host.MakeChild(host.size, {}, {}, {});
    SharedPtr<Element> anchor;
    for (const OpenPopup& popup : popups_) {
        if (popup.layer == &child) {
            anchor = popup.anchor.Lock();
            break;
        }
    }

    if (!anchor) {
        return whole;
    }

    auto anchorBounds = anchor->GetGeometry().WindowBounds();
    auto hostBounds = host.WindowBounds();
    Box2<float> work;
    AffineTransform2D inverse;
    // 기준 지점은 층이 정하거나(커서 툴팁) 앵커의 왼쪽 위다 — 여러 모니터에 걸친 창에서 UE처럼
    // 앵커 왼쪽 위가 놓인 모니터를 고른다.
    const Optional<Vec2> declared = child.PopupWorkAreaPoint();
    if (!anchorBounds || !hostBounds || !host.render.TryInverse(inverse) ||
        !QueryPopupWorkArea(declared ? *declared : anchorBounds.Value().min, work)) {
        return whole;
    }

    // 시스템 규칙(Docs/Design/UiToolkit 9.1절 UW7d): 층은 Overlay ∩ 허용 영역이다. 층은 내용을
    // 자기 경계 안에 놓고(뒤집기·밀어 넣기) 자르므로 어떤 팝업도 모니터 작업 영역을 넘지 않는다.
    const Vec2 low{math::Max(work.min.x, hostBounds.Value().min.x),
                   math::Max(work.min.y, hostBounds.Value().min.y)};
    const Vec2 high{math::Min(work.max.x, hostBounds.Value().max.x),
                    math::Min(work.max.y, hostBounds.Value().max.y)};
    if (!(high.x > low.x && high.y > low.y)) {
        return whole;
    }

    // 창 좌표 → Overlay 로컬(팝업 호스트는 회전하지 않는다 — 축 정렬 가정).
    const Vec2 localLow = inverse.Apply(low);
    const Vec2 localHigh = inverse.Apply(high);
    const Vec2 origin{math::Min(localLow.x, localHigh.x), math::Min(localLow.y, localHigh.y)};
    const Vec2 size{math::Abs(localHigh.x - localLow.x), math::Abs(localHigh.y - localLow.y)};
    return host.MakeChild(size, {1, 0, 0, 1, origin.x, origin.y}, {}, {});
}

} // namespace rocks::ui
