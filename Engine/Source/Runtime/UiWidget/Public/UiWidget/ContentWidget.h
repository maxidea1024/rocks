#pragma once

#include <UiWidget/PanelWidget.h>

namespace rocks::ui {

// 내용 하나를 감싸는 패널의 기반형이다. 두 번째 자식 추가는 CapacityExceeded를 반환한다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API ContentWidget : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ContentWidget(Allocator& allocator) noexcept;

    ObjectHandle GetContentSlot() const noexcept;
    Result<ObjectHandle> GetContent(ObjectRegistry& objects) const noexcept;
    // 빈 핸들은 내용을 비운다. 교체하면 기존 슬롯에 저장한 배치 정보가 유지된다.
    Result<void> SetContent(ObjectRegistry& objects, ObjectHandle content) noexcept;

protected:
    usize MaxChildren() const noexcept override;
};

} // namespace rocks::ui
