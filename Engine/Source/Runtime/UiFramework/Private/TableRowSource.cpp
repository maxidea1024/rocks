#include <UiFramework/TableRowSource.h>

namespace rocks::ui {

uint32 TableRowSource::ChildCount(uint64) const noexcept {
    return 0;
}

uint64 TableRowSource::ChildKey(uint64, uint32) const noexcept {
    return 0;
}

void TableRowSource::SetRowSelected(Element&, TableItem, bool) noexcept {
}

void TableRowSource::ReleaseRow(Element&, TableItem) noexcept {
}

} // namespace rocks::ui
