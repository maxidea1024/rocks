#pragma once

#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetSlot.h>

namespace rocks::ui {

// 슬롯의 실제 소유 패널과 연결 상태를 먼저 확인한다. 화면 생성 전에는 보관된 배치만
// 바꾸고, 생성 후에는 같은 자식 Element를 유지한 채 네이티브 패널 배치를 갱신한다.
template <typename Panel, typename NativePanel, typename Update>
Result<void> UpdateLiveWidgetSlot(ObjectRegistry& objects, const WidgetSlot& slot,
                                  Update&& update) noexcept {
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, slot.Handle());
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }

    auto owner = objects.Resolve<Panel>(access, outer.Value());
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }

    const auto* panel = owner.Value().Get().Value();
    usize index = 0;
    while (index < panel->ChildCount() && panel->SlotAt(index) != slot.Handle()) {
        ++index;
    }

    if (index == panel->ChildCount()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto native = panel->CachedElement();
    if (!native) {
        return Result<void>::Success();
    }

    auto child = objects.Resolve<Widget>(access, slot.Content());
    if (!child) {
        return Result<void>::Failure(child.Error());
    }

    auto element = child.Value().Get().Value()->CachedElement();
    if (!element || element->Parent() != native.Get()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    return update(static_cast<NativePanel&>(*native), *element);
}

} // namespace rocks::ui
