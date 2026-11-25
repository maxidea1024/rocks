#include <Core/ScopeExit.h>
#include <UiWidget/PanelWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetSlot.generated.h>

namespace rocks::ui {

Result<void> PanelWidget::ReplaceSingleContent(ObjectRegistry& objects,
                                               ObjectHandle next) noexcept {
    if (exchanging_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (ChildCount() == 0) {
        if (next.IsNull()) {
            return Result<void>::Success();
        }
        auto added = AddChild(objects, next);
        return added ? Result<void>::Success() : Result<void>::Failure(added.Error());
    }
    const auto access = objects.OwnerAccess();
    const auto id = SlotAt(0);
    auto slotBorrow = objects.ResolveForWrite<WidgetSlot>(access, id);
    if (!slotBorrow) {
        return Result<void>::Failure(slotBorrow.Error());
    }
    auto& slot = *slotBorrow.Value().Get().Value();
    const auto previous = slot.Content();
    if (previous == next) {
        return Result<void>::Success();
    }
    if (next.IsNull()) {
        return RemoveChild(objects, previous);
    }

    auto before = objects.ResolveForWrite<Widget>(access, previous);
    if (!before) {
        return Result<void>::Failure(before.Error());
    }
    auto after = objects.ResolveForWrite<Widget>(access, next);
    if (!after) {
        return Result<void>::Failure(after.Error());
    }
    auto& oldChild = *before.Value().Get().Value();
    auto& newChild = *after.Value().Get().Value();
    auto valid = ValidateNewChild(objects, newChild);
    if (!valid) {
        return valid;
    }

    exchanging_ = true;
    const auto guard = MakeScopeExit([this]() noexcept { exchanging_ = false; });

    // TakeElement는 사용자 OnConstruct를 호출할 수 있다. 이전 모델과 화면을 둔 채 새 화면부터
    // 만든다.
    auto panel = CachedElement();
    SharedPtr<Element> oldElement, newElement, host;
    if (panel) {
        oldElement = oldChild.CachedElement();
        if (!oldElement || !oldElement->Parent() || !oldElement->IsWithin(ChildHost(*panel))) {
            return Result<void>::Failure(ErrorCode::WrongContext);
        }
        // 정렬 슬롯은 자식 앞에 배치 노드를 둘 수 있다. 같은 노드를 유지해 여백과 정렬을 보존한다.
        auto* contentHost = oldElement->Parent();
        host = contentHost == panel.Get() ? panel : contentHost->Parent()->FindChild(contentHost);
        if (!host) {return Result<void>::Failure(ErrorCode::WrongContext);}
        auto built = newChild.TakeElement(objects);
        if (!built) {
            return Result<void>::Failure(built.Error());
        }
        newElement = Move(built.Value());
        if (panel != CachedElement() || newElement->Parent() ||
            oldElement->Parent() != host.Get() || !host->IsWithin(*panel)) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        valid = ValidateNewChild(objects, newChild);
        if (!valid) {
            return valid;
        }
    }

    // 기존 슬롯의 신원과 소유자를 보존하고 양쪽 참조를 함께 교체한다.
    auto saved = objects.SetReference(access, next, newChild.slot_, id);
    if (!saved) {
        return saved;
    }
    saved = objects.SetReference(access, id, slot.content_, next);
    if (!saved) {
        (void)objects.SetReference(access, next, newChild.slot_, {});
        return saved;
    }
    saved = objects.SetReference(access, previous, oldChild.slot_, {});
    if (saved && panel) {
        if (host->RemoveChild(*oldElement)) {
            saved = host->TryAddChild(newElement);
            if (!saved) {
                (void)host->TryAddChild(oldElement);
            }
        } else {
            saved = Result<void>::Failure(ErrorCode::WrongContext);
        }
    }
    if (!saved) {
        (void)objects.SetReference(access, previous, oldChild.slot_, id);
        (void)objects.SetReference(access, id, slot.content_, previous);
        (void)objects.SetReference(access, next, newChild.slot_, {});
        return saved;
    }
    if (panel) {
        oldChild.ReleaseElements();
    }
    return Result<void>::Success();
}

} // namespace rocks::ui
