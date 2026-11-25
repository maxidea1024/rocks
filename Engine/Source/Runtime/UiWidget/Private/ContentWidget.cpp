#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

ContentWidget::ContentWidget(Allocator& allocator) noexcept : PanelWidget(allocator) {
}

ObjectHandle ContentWidget::GetContentSlot() const noexcept {return SlotAt(0);}

Result<ObjectHandle> ContentWidget::GetContent(ObjectRegistry& objects) const noexcept {
    return ChildCount() ? GetChildAt(objects,0) : Result<ObjectHandle>::Success(ObjectHandle{});
}
Result<void> ContentWidget::SetContent(ObjectRegistry& objects,ObjectHandle content) noexcept {
    return ReplaceSingleContent(objects,content);
}

usize ContentWidget::MaxChildren() const noexcept {
    return 1;
}

} // namespace rocks::ui
