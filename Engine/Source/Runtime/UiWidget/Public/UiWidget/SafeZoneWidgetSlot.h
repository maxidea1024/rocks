#pragma once
#include <UiWidget/AlignedWidgetSlot.h>
namespace rocks::ui {
// SafeZoneWidget 자식의 배치 정보다. AddAlignedChild로 설정한 여백과 정렬은 위젯 애셋에 보존된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SafeZoneWidgetSlot final : public AlignedWidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    // 타이틀·액션 안전 영역 중 하나를 선택하고 각 변의 기기 여백에 독립적인 배율을 곱한다.
    Result<void> SetSafety(ObjectRegistry& objects, bool titleSafe, Vec4 scale) noexcept;
    Result<void> SetIsTitleSafe(ObjectRegistry& objects, bool enabled) noexcept;
    Result<void> SetSafeAreaScale(ObjectRegistry& objects, Vec4 scale) noexcept;
    bool IsTitleSafe() const noexcept;
    Vec4 SafeAreaScale() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "titleSafe") bool titleSafe_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "safeAreaScale") Vec4 safeAreaScale_ { 1, 1, 1, 1 };
};
} // namespace rocks::ui
