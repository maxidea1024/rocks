#include <Core/ScopeExit.h>
#include <Core/Text/StringBuilder.h>
#include <UiWidget/DynamicEntryBoxWidget.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

DynamicEntryBoxWidget::DynamicEntryBoxWidget(Allocator& allocator) noexcept
    : DynamicEntryBoxBaseWidget(allocator), inactive_(allocator) {
}

Result<ObjectHandle> DynamicEntryBoxWidget::CreateEntry(ObjectRegistry& objects,
                                                        ClassRef<UserWidget> type) noexcept {
    if (changing_) {
        return Result<ObjectHandle>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    const auto access = objects.OwnerAccess();
    auto cls = type.Resolve(objects.Types());
    if (!cls) {
        return Result<ObjectHandle>::Failure(cls.Error());
    }

    for (auto entry : inactive_.View()) {
        auto actual = objects.AcquireType(access, entry);
        if (!actual) {
            return Result<ObjectHandle>::Failure(actual.Error());
        }

        if (actual.Value().Schema().id == type.Id()) {
            auto attached = AddChild(objects, entry);
            if (!attached) {
                return Result<ObjectHandle>::Failure(attached.Error());
            }

            (void)objects.RemoveReference(access, Handle(), inactive_, entry);
            return Result<ObjectHandle>::Success(entry);
        }
    }

    auto outer = objects.GetOuter(access, Handle());
    if (!outer) {
        return Result<ObjectHandle>::Failure(outer.Error());
    }

    auto tree = objects.ResolveForWrite<WidgetTree>(access, outer.Value());
    if (!tree) {
        return Result<ObjectHandle>::Failure(tree.Error());
    }

    StringBuilder<> name(objects.ObjectAllocator());
    for (;;) {
        name.Clear();
        auto formatted = name.TryAppendFormat("Entry{}", nextEntry_++);
        if (!formatted) {
            return Result<ObjectHandle>::Failure(formatted.Error());
        }

        auto taken = objects.IsNameTaken(access, outer.Value(), name.View());
        if (!taken) {
            return Result<ObjectHandle>::Failure(taken.Error());
        }

        if (!taken.Value()) {
            break;
        }
    }

    auto made = tree.Value().Get().Value()->ConstructWidget(objects, type.Id(), name.View());
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const auto entry = made.Value().Handle();
    auto slot = AddChild(objects, entry);
    return slot ? Result<ObjectHandle>::Success(entry)
                : Result<ObjectHandle>::Failure(slot.Error());
}

Result<void> DynamicEntryBoxWidget::RemoveChild(ObjectRegistry& objects,
                                                ObjectHandle child) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    const auto access = objects.OwnerAccess();
    auto user = objects.Resolve<UserWidget>(access, child);
    if (!user) {
        return user.Error() == ErrorCode::TypeMismatch ? PanelWidget::RemoveChild(objects, child)
                                                       : Result<void>::Failure(user.Error());
    }

    auto retained = objects.AddReference(access, Handle(), inactive_, child);
    if (!retained) {
        return retained;
    }

    auto removed = PanelWidget::RemoveChild(objects, child);
    if (!removed) {
        (void)objects.RemoveReference(access, Handle(), inactive_, child);
    }

    return removed;
}

} // namespace rocks::ui
