#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/NamedSlotWidget.generated.h>
#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {
namespace {

Result<ObjectHandle> FindSlot(ObjectRegistry& objects, ObjectHandle treeId,
                              StringView name) noexcept {
    if (name.IsEmpty()) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }
    if (treeId.IsNull()) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }
    const auto access = objects.OwnerAccess();
    auto tree = objects.Resolve<WidgetTree>(access, treeId);
    if (!tree) {
        return Result<ObjectHandle>::Failure(tree.Error());
    }
    auto found = objects.FindChild(access, treeId, name);
    if (!found) {
        return found;
    }
    auto slot = objects.Resolve<NamedSlotWidget>(access, found.Value());
    if (!slot) {
        return Result<ObjectHandle>::Failure(
            slot.Error() == ErrorCode::TypeMismatch ? ErrorCode::NotFound : slot.Error());
    }

    // outer만 같은 미배치 위젯과 중첩 화면의 슬롯은 외부에 공개하지 않는다.
    auto reachable =
        tree.Value().Get().Value()->FindWidget(objects, slot.Value().Get().Value()->StableId());
    if (reachable && reachable.Value() != found.Value()) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }
    return reachable;
}

} // namespace

Result<Array<String>> UserWidget::GetSlotNames(ObjectRegistry& objects) const noexcept {
    using Names = Result<Array<String>>;
    auto& allocator = objects.ObjectAllocator();
    Array<String> names(allocator);
    if (Tree().IsNull()) {
        return Names::Success(Move(names));
    }
    const auto access = objects.OwnerAccess();
    auto tree = objects.Resolve<WidgetTree>(access, Tree());
    if (!tree) {
        return Names::Failure(tree.Error());
    }
    auto index = tree.Value().Get().Value()->IndexWidgets(objects);
    if (!index) {
        return Names::Failure(index.Error());
    }
    for (auto entry : index.Value()) {
        auto slot = objects.Resolve<NamedSlotWidget>(access, entry.value);
        if (!slot) {
            if (slot.Error() == ErrorCode::TypeMismatch) {
                continue;
            }
            return Names::Failure(slot.Error());
        }
        String name(allocator);
        auto got = objects.GetName(access, entry.value, name);
        if (!got) {
            return Names::Failure(got.Error());
        }
        auto appended = names.TryPushBack(Move(name));
        if (!appended) {
            return Names::Failure(appended.Error());
        }
    }
    names.Sort([](const String& a, const String& b) noexcept {
        return StringView::CompareBytes(a.View(), b.View()) < 0;
    });
    return Names::Success(Move(names));
}

Result<ObjectHandle> UserWidget::GetContentForSlot(ObjectRegistry& objects,
                                                   StringView name) const noexcept {
    auto found = FindSlot(objects, Tree(), name);
    if (!found) {
        return found;
    }
    const auto access = objects.OwnerAccess();
    auto named = objects.Resolve<NamedSlotWidget>(access, found.Value());
    if (!named) {
        return Result<ObjectHandle>::Failure(named.Error());
    }
    const auto slotId = named.Value().Get().Value()->SlotAt(0);
    if (slotId.IsNull()) {
        return Result<ObjectHandle>::Success({});
    }
    auto slot = objects.Resolve<WidgetSlot>(access, slotId);
    return slot ? Result<ObjectHandle>::Success(slot.Value().Get().Value()->Content())
                : Result<ObjectHandle>::Failure(slot.Error());
}

Result<String> UserWidget::FindSlotForContent(ObjectRegistry& objects,
                                              ObjectHandle content) const noexcept {
    if (content.IsNull() || Tree().IsNull()) {
        return Result<String>::Failure(ErrorCode::NotFound);
    }
    const auto access = objects.OwnerAccess();
    auto tree = objects.Resolve<WidgetTree>(access, Tree());
    if (!tree) {
        return Result<String>::Failure(tree.Error());
    }
    auto index = tree.Value().Get().Value()->IndexWidgets(objects);
    if (!index) {
        return Result<String>::Failure(index.Error());
    }
    for (auto entry : index.Value()) {
        auto named = objects.Resolve<NamedSlotWidget>(access, entry.value);
        if (!named) {
            if (named.Error() == ErrorCode::TypeMismatch) {
                continue;
            }
            return Result<String>::Failure(named.Error());
        }
        const auto slotId = named.Value().Get().Value()->SlotAt(0);
        if (slotId.IsNull()) {
            continue;
        }
        auto slot = objects.Resolve<WidgetSlot>(access, slotId);
        if (!slot) {
            return Result<String>::Failure(slot.Error());
        }
        if (slot.Value().Get().Value()->Content() == content) {
            String name(objects.ObjectAllocator());
            auto got = objects.GetName(access, entry.value, name);
            return got ? Result<String>::Success(Move(name)) : Result<String>::Failure(got.Error());
        }
    }
    return Result<String>::Failure(ErrorCode::NotFound);
}

Result<bool> UserWidget::ContainsContent(ObjectRegistry& objects,
                                         ObjectHandle content) const noexcept {
    auto found = FindSlotForContent(objects, content);
    if (!found && found.Error() != ErrorCode::NotFound) {
        return Result<bool>::Failure(found.Error());
    }
    return Result<bool>::Success(static_cast<bool>(found));
}

Result<void> UserWidget::SetContentForSlot(ObjectRegistry& objects, StringView name,
                                           ObjectHandle content) noexcept {
    auto found = FindSlot(objects, Tree(), name);
    if (!found) {
        return Result<void>::Failure(found.Error());
    }
    auto named = objects.ResolveForWrite<NamedSlotWidget>(objects.OwnerAccess(), found.Value());
    return named ? named.Value().Get().Value()->SetContent(objects, content)
                 : Result<void>::Failure(named.Error());
}

} // namespace rocks::ui
