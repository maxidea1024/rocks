#pragma once

#include <UiWidget/ListViewBaseWidget.h>
#include <Object/ObjectRefArray.h>

namespace rocks::ui {

// 보이는 행만 생성한다. TakeElement 전에 SetSource로 모델과 입력을 담당하는 Application을 연결한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ListViewWidget : public ListViewBaseWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ListViewWidget(Allocator& allocator) noexcept;
    using CreateEntry = Delegate<Result<SharedPtr<Element>>(Allocator&)>;
    using BindEntry = Delegate<Result<void>(Element&, ObjectHandle)>;
    // 목록이 항목 객체에 대한 강한 GC 참조를 갖는다. 화면 밖 행의 Element는 회수해 재사용한다.
    // create는 빈 행을 만들고 bind는 현재 객체에 맞춰 내용을 모두 덮어쓴다.
    Result<void> UseObjectItems(ObjectRegistry& objects, Application& app, CreateEntry&& create,
                                BindEntry&& bind) noexcept;
    Result<void> AddItem(ObjectRegistry& objects, ObjectHandle item) noexcept;
    Result<void> InsertItem(ObjectRegistry& objects, usize index, ObjectHandle item) noexcept;
    Result<void> RemoveItem(ObjectRegistry& objects, ObjectHandle item) noexcept;
    Result<void> ClearItems(ObjectRegistry& objects) noexcept;
    Span<const ObjectHandle> Items() const noexcept;
    Optional<uint32> FindItem(ObjectHandle item) const noexcept;
    Result<void> SetSelectedItem(ObjectHandle item) noexcept;
    Result<void> ScrollObjectIntoView(ObjectHandle item) noexcept;
    Result<void> SetRowHeight(float height) noexcept;

protected:
    void ApplyProperties(Element& element) noexcept override;
    Result<SharedPtr<Element>>
    CreateView(Allocator& allocator, Application& app,
               const SharedRef<TableRowSource>& source) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "items") ObjectRefArray<Object> items_;
    ROCKS_PROPERTY(ReadOnly, Name = "rowHeight") float rowHeight_ = 24;
};

} // namespace rocks::ui
