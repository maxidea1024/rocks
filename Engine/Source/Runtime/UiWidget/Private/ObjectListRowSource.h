#pragma once
#include <UiWidget/ListViewWidget.h>

namespace rocks::ui {

// 레지스트리 안에서 슬롯·세대 조합이 행의 고유 키다. 삽입·삭제로 인덱스가 바뀌어도 선택을 보존한다.
class ObjectListRowSource final : public TableRowSource {
public:
    ObjectListRowSource(ObjectRegistry& objects, ObjectHandle owner,
                        ListViewWidget::CreateEntry&& create,
                        ListViewWidget::BindEntry&& bind) noexcept;
    uint32 RootCount() const noexcept override;
    uint64 RootKey(uint32 index) const noexcept override;
    Result<SharedPtr<Element>> CreateRow(Allocator& allocator) noexcept override;
    Result<void> BindRow(Element& row, TableItem item) noexcept override;

private:
    ObjectRegistry& objects_;
    ObjectHandle owner_;
    ListViewWidget::CreateEntry create_;
    ListViewWidget::BindEntry bind_;
};
} // namespace rocks::ui
