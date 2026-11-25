#include <Base/NumericLimits.h>
#include <UiFramework/Overlay.h>

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
        auto arranged = child->ArrangeIfNeeded(GetGeometry().MakeChild(geometry.size, {}, {}, {}));
        if (!arranged) {
            return arranged;
        }
    }
    return Result<void>::Success();
}

} // namespace rocks::ui
