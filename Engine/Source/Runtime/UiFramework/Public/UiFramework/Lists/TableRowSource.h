#pragma once

#include <UiCore/Element.h>

namespace rocks::ui {

struct TableItem {
    uint64 key;
    uint32 depth;
};

// 키는 모델 안에서 유일해야 한다. 원본이 바뀌면 뷰의 Refresh를 호출한다.
class ROCKS_UIFRAMEWORK_API TableRowSource {
public:
    virtual ~TableRowSource() noexcept = default;
    virtual uint32 RootCount() const noexcept = 0;
    virtual uint64 RootKey(uint32 index) const noexcept = 0;
    virtual uint32 ChildCount(uint64 parent) const noexcept;
    virtual uint64 ChildKey(uint64 parent, uint32 index) const noexcept;
    virtual Result<SharedPtr<Element>> CreateRow(Allocator& allocator) noexcept = 0;
    virtual Result<void> BindRow(Element& row, TableItem item) noexcept = 0;
    // 모델은 이 알림에서 행의 선택 표시를 갱신한다. 재사용한 행도 새 항목의 상태를 전달받는다.
    virtual void SetRowSelected(Element& row, TableItem item, bool selected) noexcept;
    virtual void ReleaseRow(Element& row, TableItem item) noexcept;
};

} // namespace rocks::ui
