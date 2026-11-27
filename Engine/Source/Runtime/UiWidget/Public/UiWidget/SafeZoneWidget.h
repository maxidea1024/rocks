#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 뷰포트가 알려 준 안전 여백 안에 자식을 배치한다. 회전·분할 화면·디스플레이 변경 시
// 호스트는 해당 플레이어 뷰포트 기준의 논리 좌표 여백을 SetSafeInsets로 다시 넘긴다.
// 적용할 변은 위젯 애셋에, 기기별 여백은 현재 화면에만 보관한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SafeZoneWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit SafeZoneWidget(Allocator& allocator) noexcept;
    Result<void> SetSafeInsets(Vec4 insets) noexcept;
    // 타이틀(문자)과 액션(일반 표시)의 안전 여백을 따로 받은 플랫폼은 이 오버로드를 쓴다.
    Result<void> SetSafeInsets(Vec4 titleInsets, Vec4 actionInsets) noexcept;
    void SetSidesToPad(bool left, bool top, bool right, bool bottom) noexcept;

protected:
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "padLeft") bool left_ = true;
    ROCKS_PROPERTY(Name = "padTop") bool top_ = true;
    ROCKS_PROPERTY(Name = "padRight") bool right_ = true;
    ROCKS_PROPERTY(Name = "padBottom") bool bottom_ = true;
    friend class SafeZoneWidgetSlot;
    Result<void> UpdateSafety(ObjectHandle slot, bool titleSafe, Vec4 scale) noexcept;
    ROCKS_NATIVE() Vec4 titleInsets_ {};
    ROCKS_NATIVE() Vec4 actionInsets_ {};
    ROCKS_NATIVE() Vec4 scale_ { 1, 1, 1, 1 };
    ROCKS_NATIVE() bool titleSafe_ = true;
};

} // namespace rocks::ui
