#pragma once
#include <UiWidget/WidgetSlot.h>
#include <UiFramework/SlotPresenter.h>
namespace rocks::ui {
// 단일 내용, 오버레이, 페이지와 스크롤 항목에서 공유하는 여백 및 두 축 정렬이다.
// 기본 Fill과 여백 0 조합은 자식을 바로 붙여 추가 레이아웃 노드를 만들지 않는다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API AlignedWidgetSlot : public WidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetPadding(ObjectRegistry& objects, Vec4 value) noexcept;
    Result<void> SetHorizontalAlignment(ObjectRegistry& objects, StackAlignment value) noexcept;
    Result<void> SetVerticalAlignment(ObjectRegistry& objects, StackAlignment value) noexcept;

    // 연결되어 있는 슬롯의 여백과 정렬을 바꾼다. 열린 화면에서도 입력 상태와 표시 순서를 유지한다.
    Result<void> SetLayout(ObjectRegistry& objects, const SlotPlacement& placement) noexcept;
    SlotPlacement Layout() const noexcept;

protected:
    Result<void> SetLayout(const SlotPlacement& layout) noexcept;

private:
    friend class PanelWidget;
    ROCKS_PROPERTY(ReadOnly, Name = "padding") Vec4 padding_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "horizontal") uint8 horizontal_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "vertical") uint8 vertical_ = 0;
};
} // namespace rocks::ui
