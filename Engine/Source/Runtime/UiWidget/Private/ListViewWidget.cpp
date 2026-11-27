#include "MakeWidgetElement.h"
#include "ObjectListRowSource.h"
#include <UiWidget/ListViewWidget.h>
#include <UiFramework/Lists/ListView.h>

namespace rocks::ui {

ListViewWidget::ListViewWidget(Allocator& allocator) noexcept : items_(allocator) {
}

Result<void> ListViewWidget::UseObjectItems(ObjectRegistry& objects, Application& app,
                                            CreateEntry&& create, BindEntry&& bind) noexcept {
    if (CachedElement()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!create || !bind) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto source = SharedRef<ObjectListRowSource>::TryMake(objects.ObjectAllocator(), objects,
                                                          Handle(), Move(create), Move(bind));
    return source ? SetSource(app, source.Value()) : Result<void>::Failure(source.Error());
}

Span<const ObjectHandle> ListViewWidget::Items() const noexcept {
    return items_.View();
}

Optional<uint32> ListViewWidget::FindItem(ObjectHandle item) const noexcept {
    for (uint32 i = 0; i < items_.Size(); ++i) {
        if (items_.View()[i] == item) {
            return i;
        }
    }

    return {};
}

Result<void> ListViewWidget::AddItem(ObjectRegistry& objects, ObjectHandle item) noexcept {
    return InsertItem(objects, items_.Size(), item);
}

Result<void> ListViewWidget::InsertItem(ObjectRegistry& objects, usize index,
                                        ObjectHandle item) noexcept {
    if (item.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (FindItem(item)) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto added = objects.InsertReferenceAt(objects.OwnerAccess(), Handle(), items_, index, item);
    if (added) {
        Refresh();
    }

    return added;
}

Result<void> ListViewWidget::RemoveItem(ObjectRegistry& objects, ObjectHandle item) noexcept {
    auto removed = objects.RemoveReference(objects.OwnerAccess(), Handle(), items_, item);
    if (removed) {
        Refresh();
    }

    return removed;
}

Result<void> ListViewWidget::ClearItems(ObjectRegistry& objects) noexcept {
    auto cleared = objects.ClearReferences(objects.OwnerAccess(), Handle(), items_);
    if (cleared) {
        Refresh();
        ClearSelection();
    }

    return cleared;
}

Result<void> ListViewWidget::SetSelectedItem(ObjectHandle item) noexcept {
    auto i = FindItem(item);
    return i ? SelectIndex(*i) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> ListViewWidget::ScrollObjectIntoView(ObjectHandle item) noexcept {
    auto i = FindItem(item);
    return i ? ScrollIndexIntoView(*i) : Result<void>::Failure(ErrorCode::NotFound);
}

Result<SharedPtr<Element>>
ListViewWidget::CreateView(Allocator& allocator, Application& app,
                           const SharedRef<TableRowSource>& source) noexcept {
    return MakeWidgetElement<ListView>(allocator, app, source);
}

Result<void> ListViewWidget::SetRowHeight(float height) noexcept {
    if (!math::IsFinite(height) || height <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    rowHeight_ = height;
    if (auto live = CachedElement()) {
        return StaticElementCast<ListView>(*live).SetRowHeight(height);
    }

    return Result<void>::Success();
}

void ListViewWidget::ApplyProperties(Element& element) noexcept {
    ListViewBaseWidget::ApplyProperties(element);
    (void)StaticElementCast<ListView>(element).SetRowHeight(rowHeight_);
}

} // namespace rocks::ui
