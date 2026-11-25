#pragma once

#include <UiWidget/ContentWidget.h>
#include <UiFramework/FitBox.h>

namespace rocks::ui {

// 자식의 종횡비를 유지해 영역에 맞춘다. 정렬은 각 축에서 0이 시작, 1이 끝이다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ScaleBoxWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ScaleBoxWidget(Allocator& allocator) noexcept;
    Result<void> SetAlignment(Vec2 value) noexcept;
    // 맞춤 기준과 축소·확대 허용 범위를 지정한다. User 모드는 명시한 배율을 사용한다.
    Result<void> SetFit(FitMode mode, FitDirection direction = FitDirection::Both) noexcept;
    Result<void> SetUserScale(float value) noexcept;
    Vec2 Alignment() const noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "fitMode") uint8 fitMode_ = 2;
    ROCKS_PROPERTY(ReadOnly, Name = "fitDirection") uint8 fitDirection_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "userScale") float userScale_ = 1;
    ROCKS_PROPERTY(Name = "alignment") Vec2 alignment_ { 0.5f, 0.5f };
};

} // namespace rocks::ui
