#include "ObjectListRowSource.h"
#include <UiWidget/ListViewWidget.generated.h>

namespace rocks::ui {

ObjectListRowSource::ObjectListRowSource(ObjectRegistry& objects, ObjectHandle owner,
                                         ListViewWidget::CreateEntry&& create,
                                         ListViewWidget::BindEntry&& bind) noexcept
    : objects_(objects), owner_(owner), create_(Move(create)), bind_(Move(bind)) {
}

uint32 ObjectListRowSource::RootCount() const noexcept {
    auto owner = objects_.Resolve<ListViewWidget>(objects_.OwnerAccess(), owner_);
    return owner ? static_cast<uint32>(owner.Value().Get().Value()->Items().Size()) : 0;
}

uint64 ObjectListRowSource::RootKey(uint32 index) const noexcept {
    auto owner = objects_.Resolve<ListViewWidget>(objects_.OwnerAccess(), owner_);
    if (!owner) {
        return 0;
    }

    const auto items = owner.Value().Get().Value()->Items();
    if (index >= items.Size()) {
        return 0;
    }

    const auto value = items[index];
    return (static_cast<uint64>(value.slot) << 32) | value.generation;
}

Result<SharedPtr<Element>> ObjectListRowSource::CreateRow(Allocator& allocator) noexcept {
    return create_.Execute(allocator);
}

Result<void> ObjectListRowSource::BindRow(Element& row, TableItem item) noexcept {
    return bind_.Execute(
        row, {owner_.registry, static_cast<uint32>(item.key >> 32), static_cast<uint32>(item.key)});
}
} // namespace rocks::ui
