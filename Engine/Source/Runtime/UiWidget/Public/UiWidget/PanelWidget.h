#pragma once

#include <Core/Delegate.h>
#include <UiWidget/Widget.h>
#include <UiWidget/WidgetSlot.h>
#include <UiFramework/SlotPresenter.h>

namespace rocks::ui {

// 같은 WidgetTree에서 생성한 자식의 슬롯과 화면 트리 연결을 관리한다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API PanelWidget : public Widget {
    ROCKS_GENERATED_BODY()
public:
    explicit PanelWidget(Allocator& allocator) noexcept;
    virtual Result<ObjectHandle> AddChild(ObjectRegistry& objects, ObjectHandle content) noexcept;
    // 정렬 슬롯을 쓰는 패널에 여백과 정렬을 지정해 자식을 추가한다. 전용 캔버스·그리드에는 AddAt을
    // 쓴다.
    Result<ObjectHandle> AddAlignedChild(ObjectRegistry& objects, ObjectHandle child,
                                         const SlotPlacement& layout) noexcept;
    virtual Result<void> RemoveChild(ObjectRegistry& objects, ObjectHandle content) noexcept;
    usize ChildCount() const noexcept;
    // 자식 순서는 슬롯 순서다. 화면이 아직 생성되지 않은 트리에서도 조회할 수 있다.
    Result<ObjectHandle> GetChildAt(ObjectRegistry& objects, usize index) const noexcept;
    Result<Array<ObjectHandle>> GetAllChildren(ObjectRegistry& objects) const noexcept;
    Result<usize> GetChildIndex(ObjectRegistry& objects, ObjectHandle child) const noexcept;
    bool HasChild(ObjectRegistry& objects, ObjectHandle child) const noexcept;
    bool HasAnyChildren() const noexcept;
    Result<void> RemoveChildAt(ObjectRegistry& objects, usize index) noexcept;
    Result<void> ClearChildren(ObjectRegistry& objects) noexcept;
    ObjectHandle SlotAt(usize index) const noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    // 전용 슬롯의 배치 값을 먼저 기록한 뒤 GC 간선과 화면 트리를 함께 연결한다.
    // configure는 이 호출 안에서만 실행한다. 실패하면 패널의 자식 목록을 바꾸지 않는다.
    using ConfigureSlot = Delegate<Result<void>(ObjectRegistry&, ObjectHandle)>;
    Result<ObjectHandle> AddChildWithSlot(ObjectRegistry& objects, ObjectHandle content,
                                          ConfigureSlot configure) noexcept;
    // 단일 기본 슬롯의 내용과 양방향 GC 참조를 함께 바꾼다. 기존 슬롯 인스턴스는 유지한다.
    Result<void> ReplaceSingleContent(ObjectRegistry& objects, ObjectHandle content) noexcept;
    virtual Element& ChildHost(Element& panel) noexcept;
    virtual TypeId SlotType() const noexcept;
    virtual Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                                     const SharedPtr<Element>& child) noexcept;
    friend class AlignedWidgetSlot;
    virtual Result<void> UpdateAlignedSlot(ObjectRegistry& objects, ObjectHandle slot,
                                           Element& panel, Element& child,
                                           const SlotPlacement& layout) noexcept;
    virtual usize MaxChildren() const noexcept;
    virtual Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept = 0;

private:
    Result<void> ValidateNewChild(ObjectRegistry& objects, const Widget& child) const noexcept;
    ROCKS_NATIVE() bool exchanging_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "slots") ObjectRefArray<WidgetSlot> slots_;
    ROCKS_NATIVE() uint64 nextSlot_ = 0;
    ROCKS_NATIVE() ObjectRegistry* registry_ = nullptr;
};

} // namespace rocks::ui
