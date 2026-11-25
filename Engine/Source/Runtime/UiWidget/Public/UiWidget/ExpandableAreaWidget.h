#pragma once

#include <UiWidget/PanelWidget.h>

namespace rocks::ui {

// 제목을 누르면 본문이 펼쳐지는 패널이다. SetExpanded는 화면을 만들기 전에도 쓸 수 있다.
// 접힌 본문은 인스턴스와 입력 상태 값을 보존하되 측정·그리기·히트 검사에서 빠진다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ExpandableAreaWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ExpandableAreaWidget(Allocator& allocator) noexcept;

    Result<ObjectHandle> AddHeader(ObjectRegistry& objects, ObjectHandle child) noexcept;
    Result<ObjectHandle> AddBody(ObjectRegistry& objects, ObjectHandle child) noexcept;
    Result<ObjectHandle> AddChild(ObjectRegistry& objects, ObjectHandle child) noexcept override;
    bool IsExpanded() const noexcept;
    void SetExpanded(bool expanded) noexcept;
    void SetOnExpansionChanged(Delegate<void(bool)>&& callback) noexcept;

protected:
    usize MaxChildren() const noexcept override;
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    Result<ObjectHandle> AddSection(ObjectRegistry& objects, ObjectHandle child,
                                    bool header) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "expanded") bool expanded_ = false;
    ROCKS_NATIVE() Delegate<void(bool)> changed_;
};

} // namespace rocks::ui
